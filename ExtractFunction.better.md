Hey! I would like to summarize various aspects of `ExtractFunction` tweak, and propose new solutions.

Currently, the `ExtractFunction` tweak handles both the methods and free functions. I think it would be better to split
it to two different tweaks: `ExtractFreeFunction` and `ExtractMethod`. It might be that the user would like to create a
free function, while having the cursor (or selection) inside the method, thus both of the tweaks would have to be
presented in such a case.

## Scopes
`ExtractFreeFunction` shall be presented in such scopes:
* Line-wise selection inside a free function, in a header file.
* Line-wise selection inside a free function, in a source file.
* Line-wise selection inside a method, in a header file.
* Line-wise selection inside a method, in a source file.
* Expression selection, in a header file:
    * within a free function body,
    * within a method body,
    * within a constructor initializer list,
    * within a default initializer,
    * ...
* Expression selection, in a source file (same scopes as in the bullet above).
* Subexpression selection (shall work in every scope where expression selection is supported).

`ExtractMethod` shall be presented in such scopes:
* Line-wise selection inside a method, in a header file.
* Line-wise selection inside a method, in a source file.
* Expression selection, in a header file:
    * within a method body,
    * within a constructor initializer list,
    * within a default initializer,
    * ...
* Expression selection, in a source file (same scopes as in the bullet above).
* Subexpression selection (shall work in every scope where expression selection is supported).

_NOTE:_ Term "Line-wise" includes single-line selection. 

## Scope constraints

* Broken flow control. 
    * `break` statement is without a parent loop/`switch`,
    * `continue` statement is extracted without a parent loop.

## Rules

### Extract method or free function from a line-wise selection

<!-- * Trivial built-in types and pointers shall be captured by value, if not mutated within the selected area. -->
<!-- * If trivial built-in types, or pointers are mutated within the selected area -->

