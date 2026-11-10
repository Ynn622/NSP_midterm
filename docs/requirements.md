# Midterm Requirements

Implement a client–server application in C that meets all of the following requirements.

## 1. Client–Server Implementation

- The application must be implemented in C.
- The server must use `fork()` to create a child process for handling each client connection.
- Threads are not allowed.

## 2. Concurrent Clients and Runtime Proof

- The server must be able to handle at least **10 clients concurrently**.
- Runtime proof of concurrent execution must be provided.

## 3. Packet Capture and Analysis

- Provide an actual packet capture and an analysis of the captured traffic.
- Use `tcpdump` for loopback traffic, or Wireshark for communication between different computers.

## 4. Two-Level Debug Log Control

The program must support both levels of debug log control:

- **Compile time:** Control debug logging when building the program.
- **Runtime:** Control debug logging while running the program.

## 5. Dynamic Shared Library

- Utility functions shared by the client and server must be encapsulated in a dynamic shared library (`.so`).
- Both the client and server must link against that shared library.

## 6. System Information Feature

- The client must be able to request system information from the server as one of the application's features.
- Additional features may be designed and implemented.

## 7. Robustness and Execution Comparisons

- Design and implement robustness mechanisms to handle abnormal or exceptional conditions.
- Provide at least **three execution examples** demonstrating the difference in program behavior **with and without** these robustness mechanisms implemented.

## 8. Build System

- The project must be built using **Makefile or CMake**.

## 9. Submission Deliverables

The submission report must include:

1. System design explanation.
2. Description of key source code.
3. Execution screenshots.
4. Complete source code.

In addition to the report, provide a **two-minute video illustration** of the application.
