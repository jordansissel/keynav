keynav
======

Control the mouse with the keyboard.

Please see http://www.semicomplete.com/projects/keynav


Compiling
---------

You may need some extra libraries to compile keynav.  On Debian and Ubuntu you can install these packages:

    sudo apt-get install libcairo2-dev libxinerama-dev libxdo-dev

Next you simply run make:

    make

This will produce an executable `./keynav` which may be run directly (or copied
somewhere in your path). You can also install (by default directly to `/usr`)
via `make install`.
