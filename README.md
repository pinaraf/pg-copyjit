pg-copyjit
==========

Warning
-------

This code is EXPERIMENTAL!

What is this?
-------------

When JIT support was introduced in PostgreSQL, a pluggable interface was setup allowing other JIT engines to be developed.
The one integrated in PostgreSQL relies on LLVM to generate the code for a given expression.
This new engine here is based on the copy and patch paper, https://arxiv.org/pdf/2011.13127.pdf


What is the point?
------------------

The LLVM JIT engine is great for complicated queries that handle huge volumes of data, but for shorter queries that do not
involve a long runtime, the compilation time of LLVM outweights the benefits of faster execution. Also, the cost estimation
used to decide whether to compile a query or not is not always a good indicator of the possible benefits of compilation.
Thus a lot of people decided to disable JIT compilation in PostgreSQL altogether.
By using the copy and patch approach, this new compiler aims at shorter queries by having the tiniest possible compilation
time, while emitting slightly faster code than the interpreter, thus filling a gap between the interpreter and the LLVM
JIT compiler.


License
-------

Licensed under PostgreSQL license (because I reuse some PG code directly), but not done properly in repository yet...


How to build
------------

Like any PostgreSQL extension, simply issue `make`.
To use it after installing (using `make install`), just add `jit_provider='copyjit'` in your postgresql.conf.



