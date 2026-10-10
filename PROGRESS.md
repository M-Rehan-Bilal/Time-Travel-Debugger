# Time-Travel-Debugger
[October 1, 2026]
Stage 0: Receive
Implemented the complete Stack class whose template is actually provided in the way of a Singly Linked List, which serves as a live call stack during execution of the program.
Implemented the complete Timeline class which is a Doubly Linked List of Snapshots.

[October 2, 2026]
Stage 1: Pass 0x0: Validation
Implemented the function for reading Source.bin, line-by-line. The Validation function is also implemented which validates the entire program to check if the program follows the structural rules of the language or not, which mainly includes whether every function has a closing or not, nested functions are not allowed, and validates the keywords used in the program.

[October 4, 2026]
Stage 2: Pass 0x1: Resolve
Implemented the function to write Resolved Record in resolve.bin following the given format of [offset(8B)][size(4B)][string], along with the function that reads the Resolved Record from resolve.bin.
Implemented the function of resolving the program that mainly reads the source.bin file, writes the resolved record in the resolve.bin by following the format specified, and at the end replaces the function call lines offset with the actual offsets where the function exists in the resolve.bin.

[October 7, 2026]
Stage 3: Pass 0x2: Execution
Fixed a bug in Stage 2 writeResolveRecord function in which the string was dumped incorrectly in the resolve.bin due to size problem.
Implemented the function for tokenizing lines of code, one-by-one. The function of building snapshot using the given callStack is also implemented. Then executeProgram function is implemented, along with some helper functions, that reads one line from resolve.bin, and according to the keyword, performs an action. It follows the correct logic of using callStack as functions are called in the program, and the actual arguments of a function frame are updated when another function that receives that modifies it (pass-by-reference type of behavior). After executing each line, this function builds a snapshot of the program and adds it into the timeline.

[October 10, 2026]
Stage 4: Pass 0x3: Serialization
Implemented the function of writeTdbg that basically writes all the snapshots from the timeline into the session.tdbg file. This function also writes the header of tdbg, step count, index offset which tells where the index start. Index are basically at the end of the file. Index[i] is basically the offset which tells where the snapshot[i] starts in the file. Since we know the index offset later at the end, we have to update the header too. Three helper functions are also implemented to remove the repeated work. Write Frame uses write string and variable to write the entire function frame in the tdbg file, and we dump every snapshot in the session.tdbg.