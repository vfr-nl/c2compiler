
## TODO list

Parsing of:
* Balanced brackets/braces (Convert clang::BalancedDelimiterTracker)
* cast expression
* array inits via  .member = value,

AST Generation for:
* cast expression
* ...

Analysis of:
* symbol usages (Types, Vars, Functions)
* un-initialized variables
* Type comparisons
* string parsing (\n\0 etc)
* labels (redefinition)
* package+file dependencies
* ...

IR generation of:
* control flow statements (if, while, for, etc)
* global strings

C-code generation:
* saving to files
* creating build-script/Makefile

Generic:
* improve Makefile for header dependencies and .o location in subdir
* saving package info to cache
* saving recipe info to cache
* passing defines from recipe to Preprocessor
* parsing ansi-C headers into Package

Tooling:
* create c2find that only greps in files in recipe/target
* create c2style, astyle for C2
* create c2tags/c2scope that only includes files from recipe/target
* create graphical (Qt?) refactor tool to view files/packages/target symbols
    and allows project-wide renaming and drag-n-drop var/type/function definition
    reordering within files or within package.

