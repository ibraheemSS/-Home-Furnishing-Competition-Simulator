# 🏠 Home Furnishing Competition Simulator

A Linux multi-process simulation that models two teams competing to furnish two houses using processes and inter-process communication (IPC).

---

<img width="1918" height="1011" alt="image" src="https://github.com/user-attachments/assets/5b044202-6cde-49f3-ad49-74aeac879d2e" />


## 📖 Overview

This project was developed as part of an Operating Systems course. It demonstrates how multiple processes communicate and synchronize using Linux IPC mechanisms while simulating a home furnishing competition.

---

## ✨ Features

- Multi-process application
- Process creation using `fork()`
- Pipes and named FIFO
- Signal-based synchronization
- OpenMP support
- Optional OpenGL visualization

---

## 🛠️ Technologies

- C
- Linux
- POSIX API
- Pipes
- Signals
- OpenMP
- OpenGL

---

## Running the Graphical Version

### 1. Install the required packages (Ubuntu / WSL)

```bash
sudo apt update
sudo apt install build-essential
sudo apt install freeglut3-dev mesa-common-dev
```

### 2. Build the project

```bash
make clean
make graphics
```

### 3. Run the simulator

```bash
./bin/furnish config/graphics.conf
```

The simulator automatically:

- Creates the communication FIFO.
- Launches the OpenGL visualizer.
- Starts the simulation.
- Sends visualization events to the visualizer.

> **Note:** If you are using WSL, make sure GUI applications are supported (WSLg on Windows 11 or an X server such as VcXsrv on Windows 10).
