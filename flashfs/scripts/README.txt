Drop WebAssembly modules into this directory to make them available at /lfs/scripts.

The console searches these roots in order:
- /lfs/scripts
- /tmp/scripts
- /sdcard/scripts

Use the console to inspect and run them:
- script ls
- script run hello
- script run /sdcard/scripts/demo.wasm arg1 arg2

Custom host functions are exposed from the import module name "jukeboy".
See docs/scripting.md for the current host API.