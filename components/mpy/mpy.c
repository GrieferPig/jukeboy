#include "sdkconfig.h"

#ifdef CONFIG_ENABLE_TESTS
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "esp_attr.h"

#include "esp_log.h"

#include "mpy.h"

#include "port/micropython_embed.h"
#include "py/runtime.h"
#include "py/objmodule.h"

#include "py/compile.h"
#include "py/emitglue.h"
#include "py/lexer.h"
#include "py/misc.h"
#include "py/mperrno.h"
#include "py/mpprint.h"
#include "py/objmodule.h"
#include "py/parse.h"
#include "py/persistentcode.h"
#include "py/runtime.h"

static const char *TAG = "mpy";

static mp_obj_t double_it(mp_obj_t value)
{
    mp_int_t val = mp_obj_get_int(value);
    return mp_obj_new_int(val * 2);
}
static MP_DEFINE_CONST_FUN_OBJ_1(double_it_obj, double_it);

static const uint8_t custom_blob[] = {0xDE, 0xAD, 0xBE, 0xEF};

// This is example 1 script, which will be compiled and executed.
static const char *example_1 =
    "print('hello world!', list(x + 1 for x in range(10)), end='eol\\n')";

static const char *host_exports_demo =
    "print('host provided answer:', answer)\n"
    "print('double_it(21) =>', double_it(21))\n"
    "print('host greeting:', greeting)\n"
    "print('host blob len:', len(blob))\n";

// Feature validation script exercises the options enabled in mpconfigport.h.
static const char *feature_tests =
    "import gc, io, time, math, cmath\n"
    "tests = {}\n"
    "HEAP_TEST_SIZE = 20 * 1024\n"
    "def check(name, func):\n"
    "    try:\n"
    "        tests[name] = bool(func())\n"
    "    except Exception as exc:\n"
    "        print('feature', name, 'EXC', exc)\n"
    "        tests[name] = False\n"
    "buf = bytearray(b'ab')\n"
    "buf += b'c'\n"
    "check('bytearray_inplace', lambda: bytes(buf) == b'abc')\n"
    "check('bytearray_view', lambda: memoryview(buf)[0] == ord('a'))\n"
    "check('math_libm', lambda: abs(math.sin(0.5) - 0.4794255) < 1e-6)\n"
    "check('cmath_phase', lambda: abs(cmath.phase(1+1j) - 0.7853981) < 1e-6)\n"
    "check('complex_pow', lambda: abs((1+2j) ** 2) > 0)\n"
    "def check_buffered_writer():\n"
    "    bufio = io.BytesIO()\n"
    "    writer = io.BufferedWriter(bufio, 8)\n"
    "    writer.write(b'XY')\n"
    "    writer.flush()\n"
    "    return bufio.getvalue() == b'XY'\n"
    "check('io_bufferedwriter', check_buffered_writer)\n"
    "check('time_time_ns', lambda: isinstance(time.time_ns(), int))\n"
    "check('time_gmtime', lambda: time.gmtime(0)[0] == 1970)\n"
    "check('time_localtime', lambda: time.localtime(0)[0] == 1970)\n"
    "def check_gc_split():\n"
    "    big = bytearray(HEAP_TEST_SIZE)\n"
    "    return len(big) == HEAP_TEST_SIZE\n"
    "check('gc_split_heap', check_gc_split)\n"
    "gc.collect()\n"
    "for name in sorted(tests):\n"
    "    status = 'PASS' if tests[name] else 'FAIL'\n"
    "    print('feature', name, status)\n";

static const char *builtin_tests =
    "import math, builtins\n"
    "results = {}\n"
    "def _record(name, status, detail=None):\n"
    "    results[name] = (status, detail)\n"
    "def _missing_dep(deps):\n"
    "    for dep in deps:\n"
    "        if not hasattr(builtins, dep):\n"
    "            return dep\n"
    "    return None\n"
    "def _bytearray_roundtrip():\n"
    "    ba = bytearray(b'hi')\n"
    "    ba.extend(b'!')\n"
    "    return bytes(ba) == b'hi!'\n"
    "def _memoryview_edit():\n"
    "    mv = memoryview(bytearray(b'xy'))\n"
    "    mv[0] = ord('z')\n"
    "    return bytes(mv) == b'zy'\n"
    "def _set_ops():\n"
    "    return set('ab') | set('c') == set(('a', 'b', 'c'))\n"
    "def _pow_mod_supported():\n"
    "    try:\n"
    "        return pow(2, 5, 7) == 4\n"
    "    except TypeError:\n"
    "        raise NotImplementedError('pow3 unsupported')\n"
    "def run_expr(name, expr, deps=()):\n"
    "    missing = _missing_dep(deps)\n"
    "    if missing:\n"
    "        print('builtin', name, 'SKIP missing', missing)\n"
    "        _record(name, 'SKIP', 'missing ' + missing)\n"
    "        return\n"
    "    try:\n"
    "        ok = bool(eval(expr))\n"
    "    except SyntaxError as exc:\n"
    "        print('builtin', name, 'SKIP syntax', exc)\n"
    "        _record(name, 'SKIP', 'syntax ' + str(exc))\n"
    "    except NameError as exc:\n"
    "        print('builtin', name, 'SKIP missing', exc)\n"
    "        _record(name, 'SKIP', str(exc))\n"
    "    except Exception as exc:\n"
    "        print('builtin', name, 'EXC', exc)\n"
    "        _record(name, 'FAIL', str(exc))\n"
    "    else:\n"
    "        _record(name, 'PASS' if ok else 'FAIL', None if ok else 'expr false')\n"
    "def run_check(name, func, deps=()):\n"
    "    missing = _missing_dep(deps)\n"
    "    if missing:\n"
    "        print('builtin', name, 'SKIP missing', missing)\n"
    "        _record(name, 'SKIP', 'missing ' + missing)\n"
    "        return\n"
    "    try:\n"
    "        ok = bool(func())\n"
    "    except NotImplementedError as exc:\n"
    "        print('builtin', name, 'SKIP', exc)\n"
    "        _record(name, 'SKIP', str(exc))\n"
    "    except NameError as exc:\n"
    "        print('builtin', name, 'SKIP missing', exc)\n"
    "        _record(name, 'SKIP', str(exc))\n"
    "    except Exception as exc:\n"
    "        print('builtin', name, 'EXC', exc)\n"
    "        _record(name, 'FAIL', str(exc))\n"
    "    else:\n"
    "        _record(name, 'PASS' if ok else 'FAIL', None if ok else 'check false')\n"
    "def check_exception(name, func, exc_type, deps=()):\n"
    "    missing = _missing_dep(deps)\n"
    "    if missing:\n"
    "        print('builtin', name, 'SKIP missing', missing)\n"
    "        _record(name, 'SKIP', 'missing ' + missing)\n"
    "        return\n"
    "    try:\n"
    "        func()\n"
    "    except exc_type:\n"
    "        _record(name, 'PASS')\n"
    "    except Exception as exc:\n"
    "        print('builtin', name, 'EXC', exc)\n"
    "        _record(name, 'FAIL', str(exc))\n"
    "    else:\n"
    "        _record(name, 'FAIL', 'no exception')\n"
    "run_expr('abs_pow', \"abs(-3.5) == 3.5 and pow(2, 5) == 32\")\n"
    "run_check('pow_mod', _pow_mod_supported)\n"
    "run_expr('divmod_round', \"divmod(17, 3) == (5, 2) and round(2.5) == 2\")\n"
    "run_expr('sum_min_max', \"sum(range(5)) == 10 and min([3, 1, 4]) == 1 and max([3, 1, 4]) == 4\")\n"
    "run_expr('bool_int_float', \"bool(1) and int('7') == 7 and abs(float('2.5') - 2.5) < 1e-6\")\n"
    "run_expr('list_tuple_dict', \"list((1, 2)) == [1, 2] and tuple([3, 4]) == (3, 4) and {x: x * x for x in (1, 2, 3)}[3] == 9\")\n"
    "run_check('set_ops', _set_ops, ('set',))\n"
    "run_expr('any_all', \"any([0, '', 5]) and not all([1, 0, 2])\")\n"
    "run_expr('enumerate_zip', \"list(enumerate('ab', 1)) == [(1, 'a'), (2, 'b')] and list(zip([1, 2], [3, 4])) == [(1, 3), (2, 4)]\", ('enumerate', 'zip'))\n"
    "run_expr('map_filter', \"list(map(lambda x: x + 1, [1, 2, 3])) == [2, 3, 4] and list(filter(lambda x: x % 2, range(5))) == [1, 3]\", ('map', 'filter'))\n"
    "run_expr('sorted_reversed', \"sorted([3, 1, 2]) == [1, 2, 3] and list(reversed([1, 2, 3])) == [3, 2, 1]\", ('sorted', 'reversed'))\n"
    "run_expr('bytes_memoryview', \"bytes(memoryview(bytearray(b'abc'))[1:]) == b'bc'\", ('memoryview',))\n"
    "run_check('bytearray_mutation', _bytearray_roundtrip, ('bytearray', 'bytes'))\n"
    "run_check('memoryview_write', _memoryview_edit, ('memoryview', 'bytearray', 'bytes'))\n"
    "run_expr('chr_ord', \"chr(65) == 'A' and ord('A') == 65\")\n"
    "run_expr('format_str', \"format(12.5, '.1f') == '12.5' and str(True) == 'True'\", ('format',))\n"
    "run_expr('iter_next', \"(lambda it: next(it) == 1 and next(it) == 2)(iter([1, 2]))\")\n"
    "run_expr('range_list', \"list(range(2, 10, 2)) == [2, 4, 6, 8]\")\n"
    "run_expr('slice_obj', \"[1, 2, 3, 4][1:3] == [2, 3]\", ('slice',))\n"
    "run_expr('complex_numbers', \"abs(abs(complex(1, 2)) - math.sqrt(5)) < 1e-6\")\n"
    "run_expr('sum_start', \"sum([1, 2, 3], 10) == 16\")\n"
    "run_expr('type_checks', \"isinstance(True, bool) and issubclass(int, object)\")\n"
    "check_exception('zero_division', lambda: 1 / 0, ZeroDivisionError)\n"
    "for name in sorted(results):\n"
    "    status, detail = results[name]\n"
    "    if detail is None:\n"
    "        print('builtin', name, status)\n"
    "    else:\n"
    "        print('builtin', name, status, detail)\n";

static const char *custom_test =
    "import asyncio\n"
    "async def print_test():\n"
    "    while True:\n"
    "        print('test')\n"
    "        await asyncio.sleep_ms(500)\n"
    "async def main():\n"
    "    print('Asyncio is working!')\n"
    "    asyncio.create_task(print_test())\n"
    "    print('Type something and press enter to feed the native future:')\n"
    "    line = await wait_for_stdin()\n"
    "    print('Received from C:', line)\n"
    "asyncio.run(main())\n";

static void exec_script(const char *filename, const char *source)
{
    mp_embed_exec_str_with_filename(source, filename);
}

static const char *shared_models_module =
    "print('shared_models loaded')\n"
    "class Tool:\n"
    "    def __init__(self, name):\n"
    "        self.name = name\n"
    "    def label(self):\n"
    "        return 'Tool<' + self.name + '>'\n"
    "registry = []\n"
    "def register(tool):\n"
    "    registry.append(tool.label())\n"
    "def registered_labels():\n"
    "    return tuple(registry)\n";

static const char *shared_plugins_module =
    "print('shared_plugins loaded')\n"
    "from shared_models import Tool, register\n"
    "def build_suite(prefix):\n"
    "    tool = Tool(prefix + '-alpha')\n"
    "    register(tool)\n"
    "    return tool\n";

static const char *multi_file_demo =
    "from shared_models import registered_labels\n"
    "from shared_plugins import build_suite\n"
    "suite = [build_suite('multi'), build_suite('file')]\n"
    "print('multi-file tools:', [tool.label() for tool in suite])\n"
    "print('registered labels:', registered_labels())\n";

static void load_virtual_module(const char *module_name, const char *filename, const char *source)
{
    qstr module_qstr = qstr_from_str(module_name);
    mp_obj_module_t *module = MP_OBJ_TO_PTR(mp_obj_new_module(module_qstr));

    mp_obj_dict_t *prev_globals = mp_globals_get();
    mp_obj_dict_t *prev_locals = mp_locals_get();

    mp_globals_set(module->globals);
    mp_locals_set(module->globals);

    qstr source_name = qstr_from_str(filename);
    mp_lexer_t *lex = mp_lexer_new_from_str_len(source_name, source, strlen(source), 0);
    mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
    mp_obj_t module_fun = mp_compile(&parse_tree, source_name, true);
    mp_call_function_0(module_fun);

    mp_globals_set(prev_globals);
    mp_locals_set(prev_locals);
}

static const char *asyncio_future_shim =
    "import asyncio\n"
    "if not hasattr(asyncio, 'Future'):\n"
    "    from _asyncio import TaskQueue\n"
    "    from asyncio import core as _core\n"
    "    class _NativeFlag:\n"
    "        def __init__(self):\n"
    "            self.state = False\n"
    "            self.waiting = TaskQueue()\n"
    "        def set(self):\n"
    "            if self.state:\n"
    "                return\n"
    "            self.state = True\n"
    "            while self.waiting.peek():\n"
    "                _core._task_queue.push(self.waiting.pop())\n"
    "        def clear(self):\n"
    "            self.state = False\n"
    "        def wait(self):\n"
    "            if not self.state:\n"
    "                self.waiting.push(_core.cur_task)\n"
    "                _core.cur_task.data = self.waiting\n"
    "                yield\n"
    "            self.state = False\n"
    "            return True\n"
    "    class _NativeFuture:\n"
    "        def __init__(self):\n"
    "            self._flag = _NativeFlag()\n"
    "            self._value = None\n"
    "            self._exception = None\n"
    "        def set_result(self, value):\n"
    "            self._value = value\n"
    "            self._flag.set()\n"
    "        def set_exception(self, exc):\n"
    "            self._exception = exc\n"
    "            self._flag.set()\n"
    "        def _wait(self):\n"
    "            yield from self._flag.wait()\n"
    "            if self._exception is not None:\n"
    "                raise self._exception\n"
    "            return self._value\n"
    "        def __await__(self):\n"
    "            return self._wait()\n"
    "        def __iter__(self):\n"
    "            return self.__await__()\n"
    "    asyncio.Future = _NativeFuture\n";

static const char *persistent_demo_src =
    "def persisted_sum(limit):\n"
    "    raise ValueError('Test')\n"
    "    return sum(range(limit))\n"
    "print('persisted_sum via MPY:', persisted_sum(5))\n";
static const char *persistent_demo_filename = "persistent_demo.py";

static void run_persistent_demo(void)
{
    qstr source_name = qstr_from_str(persistent_demo_filename);
    mp_lexer_t *lex = mp_lexer_new_from_str_len(source_name, persistent_demo_src, strlen(persistent_demo_src), 0);
    mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
    mp_module_context_t ctx = {0};
    ctx.module.globals = mp_globals_get();
    mp_compiled_module_t cm = {0};
    cm.context = &ctx;
    mp_compile_to_raw_code(&parse_tree, source_name, false, &cm);

    vstr_t vstr;
    mp_print_t print;
    vstr_init_print(&vstr, 64, &print);
    mp_raw_code_save(&cm, &print);
    mp_parse_tree_clear(&parse_tree);

    printf("Running persisted MPY (%zu bytes)\n", (size_t)vstr.len);
    mp_embed_exec_mpy((const uint8_t *)vstr.buf, vstr.len);
    vstr_clear(&vstr);
}

typedef struct _stdin_future_req_t
{
    mp_obj_t future;
    char *line;
    size_t len;
} stdin_future_req_t;

static mp_obj_t stdin_future_complete(mp_obj_t ptr_obj);
static MP_DEFINE_CONST_FUN_OBJ_1(stdin_future_complete_obj, stdin_future_complete);

static mp_obj_t stdin_future_complete(mp_obj_t ptr_obj)
{
    stdin_future_req_t *req = (stdin_future_req_t *)(uintptr_t)mp_obj_get_int(ptr_obj);
    mp_obj_t dest[3];
    if (req->line == NULL)
    {
        mp_load_method(req->future, qstr_from_str("set_exception"), dest);
        dest[2] = mp_obj_new_exception(&mp_type_EOFError);
        mp_call_method_n_kw(1, 0, dest);
    }
    else
    {
        mp_load_method(req->future, qstr_from_str("set_result"), dest);
        dest[2] = mp_obj_new_str(req->line, req->len);
        mp_call_method_n_kw(1, 0, dest);
    }
    if (req->line != NULL)
    {
        free(req->line);
    }
    free(req);
    return mp_const_none;
}

static void *stdin_reader_thread(void *arg)
{
    stdin_future_req_t *req = (stdin_future_req_t *)arg;

    // Automatically generate a line after a short delay instead of reading stdin.
    const char *auto_line = "auto-generated-value";
    usleep(1000 * 1000); // 1 second delay in microseconds.

    req->len = strlen(auto_line);
    req->line = strdup(auto_line);
    if (req->line == NULL)
    {
        req->len = 0;
    }
    mp_sched_schedule(MP_OBJ_FROM_PTR(&stdin_future_complete_obj),
                      mp_obj_new_int_from_ull((uintptr_t)req));
    return NULL;
}

static mp_obj_t wait_for_stdin(void)
{
    mp_obj_t asyncio = mp_import_name(qstr_from_str("asyncio"), mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    mp_obj_t dest[2];
    mp_obj_t loop;

    mp_load_method_maybe(asyncio, qstr_from_str("get_running_loop"), dest);
    if (dest[0] != MP_OBJ_NULL)
    {
        loop = mp_call_method_n_kw(0, 0, dest);
    }
    else
    {
        mp_load_method(asyncio, qstr_from_str("get_event_loop"), dest);
        loop = mp_call_method_n_kw(0, 0, dest);
    }

    mp_obj_t future;
    mp_load_method_maybe(loop, qstr_from_str("create_future"), dest);
    if (dest[0] != MP_OBJ_NULL)
    {
        future = mp_call_method_n_kw(0, 0, dest);
    }
    else
    {
        mp_obj_t future_type = mp_load_attr(asyncio, qstr_from_str("Future"));
        future = mp_call_function_0(future_type);
    }

    stdin_future_req_t *req = (stdin_future_req_t *)calloc(1, sizeof(stdin_future_req_t));
    if (req == NULL)
    {
        mp_raise_OSError(MP_ENOMEM);
    }
    req->future = future;

    pthread_t reader;
    int err = pthread_create(&reader, NULL, stdin_reader_thread, req);
    if (err != 0)
    {
        free(req);
        mp_raise_OSError(err);
    }
    pthread_detach(reader);

    return future;
}
static MP_DEFINE_CONST_FUN_OBJ_0(wait_for_stdin_obj, wait_for_stdin);

// MicroPython GC heap — placed in PSRAM to save internal DRAM.
EXT_RAM_BSS_ATTR static char heap[8 * 1024];

void mpy_run_demo()
{
    // Initialise MicroPython.
    //
    // Note: &stack_top below should be good enough for many cases.
    // However, depending on environment, there might be more appropriate
    // ways to get the stack top value.
    // eg. pthread_get_stackaddr_np, pthread_getattr_np,
    // __builtin_frame_address/__builtin_stack_address, etc.
    int stack_top;
    mp_embed_init(&heap[0], sizeof(heap), &stack_top);

    // Provide a few host-side objects to the interpreter before running scripts.
    mp_embed_set_global("answer", mp_embed_new_int(42));
    mp_embed_set_global("greeting", mp_embed_new_str("hello from C"));
    mp_embed_set_global("blob", mp_embed_new_bytes(custom_blob, sizeof(custom_blob)));
    mp_embed_set_global("double_it", MP_OBJ_FROM_PTR(&double_it_obj));
    mp_embed_set_global("wait_for_stdin", MP_OBJ_FROM_PTR(&wait_for_stdin_obj));

    // Run the example scripts (they will be compiled first).
    exec_script("example_1.py", example_1);
    exec_script("host_exports_demo.py", host_exports_demo);
    exec_script("sys_probe.py",
                "import sys\n"
                "dir(sys)\n"
                "print(sys.platform)\n"
                "mods = getattr(sys, 'builtin_module_names', tuple(sorted(sys.modules.keys())))\n"
                "print(mods)\n");
    load_virtual_module("shared_models", "shared_models.py", shared_models_module);
    load_virtual_module("shared_plugins", "shared_plugins.py", shared_plugins_module);
    exec_script("multi_file_demo.py", multi_file_demo);
    exec_script("feature_tests.py", feature_tests);
    exec_script("builtin_tests.py", builtin_tests);
    exec_script("asyncio_future_shim.py", asyncio_future_shim);
    exec_script("asyncio_demo.py", custom_test);

    run_persistent_demo();

    // Deinitialise MicroPython.
    mp_embed_deinit();
    ESP_LOGI(TAG, "MicroPython demo finished");
}

#endif // CONFIG_ENABLE_TESTS