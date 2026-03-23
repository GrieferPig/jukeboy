Example of embedding MicroPython in a standalone C application
==============================================================

This directory contains a simple example of how to embed MicroPython in an
existing C application.

A C application is represented here by the file `main.c`.  It executes two
simple Python scripts which print things to the standard output.

Building the example
--------------------

First build the embed port using:

    $ make -f micropython_embed.mk

This will generate the `micropython_embed` directory which is a self-contained
copy of MicroPython suitable for embedding.  The .c files in this directory need
to be compiled into your project, in whatever way your project can do that.  The
example here uses make and a provided `Makefile`.

To build the example project, based on `main.c`, use:

    $ make

That will create an executable called `embed` which you can run:

    $ ./embed

Injecting host objects
----------------------

The helper header `port/micropython_embed.h` now exposes utilities for
creating MicroPython objects from C and publishing them directly to the
interpreter's global namespace at runtime:

```c
mp_embed_set_global("answer", mp_embed_new_int(42));
mp_embed_set_global("greeting", mp_embed_new_str("hello from C"));
mp_embed_set_global("blob", mp_embed_new_bytes(data, sizeof(data)));
mp_embed_set_global("double_it", MP_OBJ_FROM_PTR(&double_it_obj));
```

Any object that can be represented as an `mp_obj_t` (numbers, strings, bytes,
or even custom C functions wrapped with `MP_DEFINE_CONST_FUN_OBJ_x`) can be
exposed on demand without editing the MicroPython configuration. Strings and
bytes passed to the helpers are copied into MicroPython-managed memory, so the
original buffers can be stack-allocated. When exposing callables, include
`py/runtime.h` so that helpers like `mp_obj_get_int` are available while writing
your function bodies.

Out of tree build
-----------------

This example is set up to work out of the box, being part of the MicroPython
tree.  Your application will be outside of this tree, but the only thing you
need to do for that is to change `MICROPYTHON_TOP` (found in `micropython_embed.mk`)
to point to the location of the MicroPython repository.  The MicroPython
repository may, for example, be a git submodule in your project.
