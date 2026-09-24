# randomizer_module

An out-of-tree, loadable Linux kernel module that

1. logs a message to the kernel log on load and unload,
2. exposes a character device (`/dev/module_device`) that accepts text from userspace and stores the words in dynamic kernel memory,
3. returns all currently stored words when the device is read,
4. watches a GPIO pin (chosen with a module parameter) via interrupt and logs a randomly selected stored word every time the pin goes high.

The GPIO chip is simulated with the `gpio-sim` kernel module, so no real hardware is needed.

## Assumptions

The task leaves some details open. These are the interpretations I chose:

- **Target environment:** Ubuntu 22.04 with kernel 6.8. The kernel version is my own choice; `gpio-sim` requires 5.17 or newer.
- **"File interface":** a character device at `/dev/module_device`. It is root-only by default, so the examples use `sudo`.
- **"Words":** whitespace-separated tokens. Spaces, tabs and newlines all act as separators, and empty tokens are ignored.
- **Storing:** every write appends its words to the list. Duplicates are allowed and there is no way to remove words except unloading the module.
- **Reading:** returns all stored words, one per line, in the order they were written.
- **Input size:** a single write must be smaller than 4096 bytes; larger writes are rejected. This limit is my own choice, to bound how much memory one call can allocate.
- **"Pin goes high":** a rising edge (0 to 1 transition), not the high level. Each rising edge logs exactly one word.
- **Empty list:** if the pin goes high while no words are stored, nothing is logged.
- **Pin selection:** the `gpio_pin` parameter is the *global* GPIO number as shown in `/sys/kernel/debug/gpio`. Without it, the module refuses to load (`-EINVAL`), since there is no sensible default pin.
- **Randomness:** the kernel's `get_random_u32()` is used. The selection only has to look random; no cryptographic guarantees are needed.
- **GPIO API:** the integer-based (legacy) GPIO API is acceptable for this task, since it works directly with a pin number given as a module parameter.
- **Simulation:** the GPIO chip is provided by `gpio-sim`, set up through configfs with one line. The pin is toggled through the `pull` sysfs attribute of the simulated line.

**Tested on:** Ubuntu 22.04.5 LTS, kernel 6.8.0-138-generic
(`gpio-sim` requires kernel 5.17 or newer.)

## Repository contents

| File | Purpose |
|---|---|
| `randomizer_module.c` | The module source |
| `Makefile` | Kbuild-based build (out-of-tree module) |
| `test.sh` | Reproducible test run that produces the example log |
| `example_kernel.log` | Kernel log from a run of `test.sh` |

## Build

Needs the kernel headers for the running kernel (on Ubuntu: `sudo apt install linux-headers-$(uname -r)`).

```bash
make
```

This calls the kernel build system (`make -C /lib/modules/$(uname -r)/build M=$(PWD) modules`) and produces `randomizer_module.ko`.

## Usage

### 1. Set up the simulated GPIO chip

```bash
sudo modprobe gpio-sim
mountpoint -q /sys/kernel/config || sudo mount -t configfs none /sys/kernel/config
sudo mkdir -p /sys/kernel/config/gpio-sim/sim0/bank0
echo 1 | sudo tee /sys/kernel/config/gpio-sim/sim0/bank0/num_lines
echo 1 | sudo tee /sys/kernel/config/gpio-sim/sim0/live
```

### 2. Find the global GPIO number

The `gpio_pin` parameter takes the *global* GPIO number, which depends on the machine and changes when other GPIO chips are registered:

```bash
sudo cat /sys/kernel/debug/gpio
```

Look for the chip whose parent is `platform/gpio-sim.0`. Its first GPIO is line 0. For example, `gpiochip2: GPIOs 936-936, parent: platform/gpio-sim.0` means `gpio_pin=936`. Never use the range of a real chip.

### 3. Load the module and use it

```bash
sudo insmod ./randomizer_module.ko gpio_pin=936

# store words (any whitespace separates words; repeated writes append)
echo "hello world fritz nala" | sudo tee /dev/module_device

# read the stored words, one per line
sudo cat /dev/module_device

# generate a rising edge on the simulated pin
P=/sys/devices/platform/gpio-sim.0/gpiochip2/sim_gpio0/pull
echo pull-down | sudo tee $P
echo pull-up   | sudo tee $P

sudo dmesg | grep "Random word"

sudo rmmod randomizer_module
```

The sysfs path contains the chip number (`gpiochip2` here); adjust it to your system.

**Note:** an interrupt is only generated on a 0 to 1 transition. Writing `pull-up` to a line that is already high does nothing, so always go `pull-down` first.

### Automated test

`test.sh` runs the full cycle: an error case (loading without `gpio_pin`), loading with the pin, writing 16 words, reading them back, 30 rising edges, and unloading. It writes the filtered kernel log to `example_kernel.log`.

## Design decisions

### Character device via `miscdevice`

The task asks for a file interface. I used a misc device instead of a full `cdev` setup (`alloc_chrdev_region`, `cdev_add`, `class_create`, `device_create`). A misc device needs a single `misc_register()` call, creates the `/dev/` node automatically, and is the usual choice for a simple single-device driver. `MISC_DYNAMIC_MINOR` lets the kernel pick a free minor number, so there is no conflict with other drivers. By default the node is only accessible by root.

### Storage: kernel linked list

Each word is a `struct` with a heap-allocated string and a `struct list_head`, kept in a list built with the kernel's own `<linux/list.h>`. The number of words is not known in advance and words are only appended and iterated, so a linked list fits better than a fixed-size array, and it grows as needed. Memory comes from `kmalloc`/`kstrdup` with `GFP_KERNEL`, which is correct because writes run in process context where sleeping is allowed.

### Parsing the written text

The written buffer is copied into kernel memory with `copy_from_user()` (a userspace pointer must never be dereferenced directly) and split at spaces, tabs and newlines using `strsep()`. Empty tokens (e.g. from double spaces) are skipped. Input is limited to `BUFFER_SIZE` (4096) bytes per write, so a userspace program cannot make the module allocate arbitrary amounts of memory in a single call; larger writes are rejected with `-EINVAL`.

### Reading

On a read at offset 0, the module builds one output string containing all words (one per line) and then serves it in chunks using the file offset, copying with `copy_to_user()`. That way `cat` and partial reads with small buffers both work correctly.

### Locking: one mutex

The word list is used from two contexts: file operations (`read`/`write`) and the interrupt thread. A single `mutex` protects the list and the output buffer. A mutex is enough because both contexts are allowed to sleep, so a spinlock is not needed (see the next section).

### Interrupt handling: threaded IRQ

The handler needs to take the mutex and walk the list, and mutexes may sleep. A normal hard-IRQ handler is not allowed to sleep, so I used `request_threaded_irq()` with no primary handler (`NULL`) and the work in the thread function. For this case (no primary handler) the kernel requires `IRQF_ONESHOT`, so the line stays masked until the thread has finished.

The trigger is `IRQF_TRIGGER_RISING`: the task says to log "every time the pin becomes high", which is exactly a rising edge. A level trigger (`IRQF_TRIGGER_HIGH`) would fire continuously while the pin stays high.

### Random selection

The handler counts the entries, picks an index with `get_random_u32() % count`, walks to that entry and logs it with `pr_info()`. If the list is empty, nothing is logged. The modulo introduces a negligible bias for such small lists.

### GPIO API

I used the classic integer-based GPIO API (`gpio_request`, `gpio_direction_input`, `gpio_to_irq`). It is considered legacy in favor of the descriptor-based `gpiod_*` API, but a module that is not bound to a device-tree or ACPI node has no natural way to look up a descriptor, and the integer API works directly with a pin number passed as a module parameter, which is what the task asks for. Switching to `gpiod_*` would need either a lookup table or `gpio_to_desc()`.

### Module parameter

`gpio_pin` is declared with `module_param(gpio_pin, int, 0444)`: it can be set at load time and is visible read-only in `/sys/module/randomizer_module/parameters/`. The default is `-1`, which means "not specified": loading then fails with `-EINVAL`, because there is no sensible default pin.

### Error handling and cleanup

`module_init` sets up resources in a fixed order (misc device, GPIO, IRQ). If a step fails, `goto` labels undo only what was already set up, in reverse order. This is the standard pattern in kernel code and avoids duplicating cleanup in every error branch. The `gpio_pin` check happens before anything is registered.

`module_exit` frees the IRQ and the GPIO, then the word list and the output buffer, then deregisters the device. Because `.owner = THIS_MODULE` is set, the module cannot be unloaded while the device file is open.

## Known limitations

- The word list can only be cleared by unloading the module; there is no "reset" command.
- The output buffer is shared. Two processes reading the device at the same time can interfere with each other. Using the `seq_file` interface would remove this.
- Words are appended without checking for duplicates.
- If an allocation fails halfway through a write, the words stored so far are kept and the write still reports full success.
- The legacy GPIO API is used (see above).
