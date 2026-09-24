#!/bin/bash
MOD=randomizer_module
PIN=936
P=/sys/devices/platform/gpio-sim.0/gpiochip2/sim_gpio0/pull
WORDS="hello world fritz nala gigi aang katara sokka toph momo appa zuko iroh azula ozai melonlord"

mark() { echo "test: $1" | sudo tee /dev/kmsg > /dev/null; }

sudo rmmod $MOD 2>/dev/null
sudo dmesg -C

mark "load without gpio_pin (must fail)"
sudo insmod ./$MOD.ko 2>/dev/null

mark "load with gpio_pin=$PIN"
sudo insmod ./$MOD.ko gpio_pin=$PIN

mark "write words"
echo "$WORDS" | sudo tee /dev/module_device > /dev/null
sudo cat /dev/module_device

mark "30 rising edges"
for i in $(seq 1 30); do
    echo pull-down | sudo tee $P > /dev/null
    echo pull-up   | sudo tee $P > /dev/null
    sleep 0.3
done

mark "unload"
sudo rmmod $MOD

sudo dmesg | grep -E "test:|Module (loaded|unloaded)|Random word|Error:" > example_kernel.log
cat example_kernel.log
