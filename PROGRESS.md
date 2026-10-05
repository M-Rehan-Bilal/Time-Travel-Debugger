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