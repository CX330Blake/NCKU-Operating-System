# 🧠 OS 2025 Lab 1 – Shared Memory & Message Passing

---

## 🧩 1. Overview

### Inter-Process Communication (IPC)

In this lab, you will implement and compare two IPC mechanisms:

- **Message Passing**
- **Shared Memory**

### Communication Flow Diagram

| Sender State | Receiver State | Action                             |
| ------------ | -------------- | ---------------------------------- |
| Running      | Blocked        | ① Send data                        |
| Blocked      | Running        | ② Receive data                     |
| —            | —              | Repeat the red cycle until the end |

---

## ⚙️ 2. Requirements & Flow

### (1) Main Task

Implement communication between a **sender** and a **receiver** process.

#### You must implement two wrapper functions

| File         | Function                      |
| ------------ | ----------------------------- |
| `sender.c`   | `send(message, &mailbox)`     |
| `receiver.c` | `receive(&message, &mailbox)` |

---

### (2) Communication Mechanisms

You must support **two mechanisms**:

1. **Message Passing** (using message queue system calls)
2. **Shared Memory** (using shared memory system calls)

---

### (3) `sender.c` – `main()`

#### Steps

1. Call `send(message, &mailbox)` following the flow shown in slide 4
2. Measure the **total sending time**
3. Read the **mechanism type** and **input file** from command-line arguments:

    ```bash
    ./sender 1 input.txt
    ```

    - `1`: Message Passing
    - `2`: Shared Memory

4. Read messages line by line from the input file
5. Print message information according to the required output format
6. When the input message is `EOF`, send an **exit message** to the receiver
7. Print the **total sending time** and terminate the sender process

---

### (4) `receiver.c` – `main()`

#### Steps

1. Call `receive(&message, &mailbox)` following the same communication flow
2. Measure the **total receiving time**
3. Get the mechanism type from the command-line argument:

    ```bash
    ./receiver 1
    ```

4. Print message information according to the required output format
5. When an **exit message** is received, print the total receiving time and terminate the receiver process

---

### (5) Mailbox Structure

The TAs will provide a mailbox structure to be used in your implementations:

```c
typedef struct {
    int flag;       // Specifies the mechanism (Message Passing / Shared Memory)
    void *storage;  // Points to the mailbox (message queue / shared memory)
} mailbox_t;
```

---

### (6) Input File Format

- Each line represents a message
- Message size: **1–1024 bytes**
- No blank lines

---

### (7) Output Format

The console output must follow the **given format** provided by the TAs.
It should display relevant information for both sending and receiving.

---

## ⏱️ 3. Time Measurement

Only measure the **communication-related operations**, such as:

- ✅ Sending/receiving messages via **Message Passing system calls**
- ✅ Accessing or updating **Shared Memory**

Do **not** measure unrelated actions:

- ❌ Waiting to be unblocked
- ❌ Printing or I/O unrelated to IPC

---

## 📚 4. Related Works

### Semaphore

Used for **synchronization** between processes.
A semaphore `S` is an integer variable that can be modified by two standard operations:

```c
wait(S);
signal(S);
```

---

### Deadlock

Occurs when two or more processes wait indefinitely for events that only other waiting processes can trigger.

Example:

- Process P0 executes `wait(S)` then `wait(Q)`
- Process P1 executes `wait(Q)` then `wait(S)`
  → Both become stuck, leading to deadlock.

---

### Initialization Deadlock

If a semaphore is mistakenly initialized to **0**, no process can proceed — causing a deadlock at startup.

---

### Common APIs Reference

| Category            | System V API                                   | POSIX API                                                               |
| ------------------- | ---------------------------------------------- | ----------------------------------------------------------------------- |
| **Semaphore**       | `semget()`, `semop()`, `semctl()`              | `sem_open()`, `sem_wait()`, `sem_post()`, `sem_close()`, `sem_unlink()` |
| **Shared Memory**   | `shmget()`, `shmat()`, `shmdt()`, `shmctl()`   | `shm_open()`, `mmap()`, `munmap()`, `shm_unlink()`                      |
| **Message Passing** | `msgget()`, `msgsnd()`, `msgrcv()`, `msgctl()` | `mq_open()`, `mq_send()`, `mq_receive()`, `mq_close()`, `mq_unlink()`   |

---

## 🎯 5. Demo & Grading

| Item                          | Score   | Description                                          |
| ----------------------------- | ------- | ---------------------------------------------------- |
| Message Passing Communication | 2.5 pts | Show correct output based on message passing         |
| Shared Memory Communication   | 2.5 pts | Show correct output based on shared memory           |
| Performance Comparison        | 2 pts   | Compare performance (Shared Memory should be faster) |
| Code Understanding Questions  | 3 pts   | Answer 3 TA questions about your code                |

---

### ⚠️ Notes

- Implement this lab in **C language**
- You will receive **6 files**:

    ```
    sender.c
    sender.h
    receiver.c
    receiver.h
    message.txt
    makefile
    ```

- You can modify the `makefile`, but it **must compile successfully** and generate working executables.

**GitHub Template:**
👉 [https://github.com/1y1c0c8/os_2025_lab1_template.git](https://github.com/1y1c0c8/os_2025_lab1_template.git)
