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

## 📂 Repository Structure

```text
.
├── src/
├── include/
├── config/
├── docs/
│   ├── Project_Paper.pdf
│   ├── Presentation.pdf
│   └── images/
├── Makefile
└── README.md
```

---

## 📚 Documentation

- 📄 Project Paper: `docs/Project_Paper.pdf`
- 📊 Project Presentation: `docs/Presentation.pdf`

---

## ▶️ Build

```bash
make
```

---

## ▶️ Run

```bash
./bin/furnish
```

---

