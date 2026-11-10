# Time-Travel Debugger (TTDB)

The Time-Travel Debugger lets you inspect a program's execution history even after it has finished running. Instead of restarting a program to catch an error at the right moment, TTDB records its state throughout execution. You can then move backward or forward through the recorded steps to see how variables changed and how function calls affected the program.

Programs are written in C--, a lightweight custom language with eight instructions: func, func_end, call, set, add, sub, mul, and div. It supports function calls, parameter passing, and copy-back semantics, so changes to function parameters are reflected in the caller when the function returns.

The project consists of three components: a client, a load balancer, and a server. The client uploads the source program to the load balancer, which queues requests in first-come, first-served order and forwards them to the server. The server validates and resolves the program, executes it using a call stack, and captures a snapshot after each executed instruction. It then serializes the snapshots into a .tdbg file and sends it back.

Once downloaded, the client works offline. Commands such as next, prv, and jmp navigate the execution history using a dense index for direct access to snapshots. Network messages are protected by CRC-32 checksums, and the client validates the downloaded file before replacing an existing debugging session.
