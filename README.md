# Linux Kernel Module: Dynamic Word Buffer & GPIO Event MonitorThis project implements a loadable Linux kernel module that bridges kernel-space and user-space through a file interface and interacts with hardware interrupts via GPIO

## Features
- Lifecycle Logging: Emits descriptive log messages to the kernel ring buffer (dmesg) upon loading and unloading.
- User-Space Data Exchange: Provides a dedicated file interface allowing user-space processes to send text data, which is stored in a dynamically managed kernel-space memory buffer.
- Word Retrieval: Reading from the file interface returns the currently stored words.
- GPIO Interrupt Monitoring: Monitors a configurable GPIO pin via an Interrupt Request (IRQ). Whenever the pin transitions to the HIGH state, a randomly selected word from the buffer is logged to the kernel ring buffer.
- Simulation Support: Fully compatible with gpio-sim (available in Linux kernel $\ge 5.17$), allowing GPIO and interrupt testing without physical hardware.
