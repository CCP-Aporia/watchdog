/**
 * _fsevents.c: Low-level FSEvents Python API.
 *
 */
#include "Python.h"

#include <stdlib.h>
#include <signal.h>
#include <limits.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>

/**
 * Py_ssize_t type for Python versions that don't define it.
 */
#if PY_VERSION_HEX < 0x02050000 && !defined(PY_SSIZE_T_MIN)
typedef int Py_ssize_t;
#define PY_SSIZE_T_MAX INT_MAX
#define PY_SSIZE_T_MIN INT_MIN
#endif /* PY_VERSION_HEX && !PY_SSIZE_T_MIN */

const char *MODULE_NAME = "_fsevents";
const char *MODULE_CONSTANT_NAME_POLLIN = "POLLIN";
const char *MODULE_CONSTANT_NAME_POLLOUT = "POLLOUT";

/**
 * Error messages.
 */
const char *CALLBACK_ERROR_MESSAGE = "Unable to call callback function.";

/**
 * Module documentation.
 */
static const char *MODULE_DOCUMENTATION = "Low-level FSEvents interface.";


/**
 * Dictionary of all the event loops.
 */
PyObject *g__pydict_loops = NULL;

/**
 * Dictionary of all the event streams.
 */
PyObject *g__pydict_streams = NULL;

/**
 * Filesystem event stream meta information structure.
 */
typedef struct {
    /**
     * Callback called when an event is triggered with the event paths and masks
     * as arguments.
     */
    PyObject *callback_event_handler;

    /**
     * Event stream.
     */
    FSEventStreamRef stream;

    /**
     * Loop associated with the event stream.
     */
    CFRunLoopRef loop;

    /**
     *
     */
    PyThreadState *thread_state;
} FSEventStreamInfo;


/**
 * Handles streamed events and calls the callback defined in Python code.
 *
 * @param stream       The stream of events.
 * @param info         Meta information about the stream of events.
 * @param num_events   Number of events.
 * @param event_paths  The paths on which events occurred.
 * @param event_masks  The masks for each of the paths in `event_paths'.
 * @param event_ids    Event identifiers.
 *
 * @synchronized()
 */
static void
event_stream_handler (FSEventStreamRef stream,
                      FSEventStreamInfo *stream_info,
                      const int num_events,
                      const char *const event_paths[],
                      const FSEventStreamEventFlags *event_masks,
                      const uint64_t *event_ids)
{
    PyThreadState *saved_thread_state = NULL;
    PyObject *event_path = NULL;
    PyObject *event_mask = NULL;
    PyObject *event_path_list = NULL;
    PyObject *event_mask_list = NULL;
    int i = 0;


    /* Acquire lock and save thread state. */
    PyEval_AcquireLock();
    saved_thread_state = PyThreadState_Swap(stream_info->thread_state);


    /* Create Python lists that will contain event paths and masks. */
    event_path_list = PyList_New(num_events);
    event_mask_list = PyList_New(num_events);
    if (!(event_path_list && event_mask_list))
        {
            return NULL;
        }

    /* Enumerate event paths and masks into python lists. */
    for (i = 0; i < num_events; ++i)
        {
            event_path = PyString_FromString(event_paths[i]);
            event_mask = PyInt_FromLong(event_masks[i]);
            if (!(event_mask && event_path))
                {
                    Py_DECREF(event_path_list);
                    Py_DECREF(event_mask_list);
                    return NULL;
                }
            PyList_SET_ITEM(event_path_list, i, event_path);
            PyList_SET_ITEM(event_mask_list, i, event_mask);
        }

    /* Call the callback event handler function with the enlisted event masks and paths as arguments.
     * On failure check whether an error occurred and stop this instance of the runloop.
     */
    if (PyObject_CallFunction(stream_info->callback_event_handler, "OO", event_path_list, event_mask_list) == NULL)
        {
            /* An exception may have occurred. */
            if (!PyErr_Occurred())
                {
                    /* If one didn't occur, raise an exception informing that we could not execute the
                       callback function. */
                    PyErr_SetString(PyExc_ValueError, CALLBACK_ERROR_MESSAGE);
                }

            /* Stop listening for events. */
            CFRunLoopStop(stream_info->loop);
        }


    /* Restore original thread state and release lock. */
    PyThreadState_Swap(saved_thread_state);
    PyEval_ReleaseLock();
}


/**
 * Runs an event loop in a thread.
 *
 * @param self Python 'self'.
 * @param args Arguments tuple.
 *
 * @return None
 */
static char pyfsevents_loop_doc[] = "Runs an event loop in a thread.";
static PyObject *
pyfsevents_loop (PyObject *self,
                 PyObject *args)
{
    PyObject *thread = NULL;
    PyObject *value = NULL;

    if (!PyArg_ParseTuple(args, "O:loop", &thread))
        {
            return NULL;
        }

    PyEval_InitThreads();

    /* Allocate info object and store thread state. */
    value = PyDict_GetItem(g__pydict_loops, thread);
    if (value == NULL)
        {
            CFRunLoopRef loop = CFRunLoopGetCurrent();
            value = PyCObject_FromVoidPtr((void *)loop, PyMem_Free);
            PyDict_SetItem(g__pydict_loops, thread, value);
            Py_INCREF(thread);
            Py_INCREF(value);
        }


    /* No timeout, block until events. */
    Py_BEGIN_ALLOW_THREADS;
    CFRunLoopRun();
    Py_END_ALLOW_THREADS;


    /* Clean up state information data. */
    if (PyDict_DelItem(g__pydict_loops, thread) == 0)
        {
            Py_DECREF(thread);
            Py_INCREF(value);
        }

    if (PyErr_Occurred())
        {
            return NULL;
        }


    Py_INCREF(Py_None);
    return Py_None;
}


/**
 * Schedules a stream.
 *
 * @param self
 *     Python 'self'.
 * @param args
 *     Arguments tuple.
 *
 * @return None
 */
static char pyfsevents_schedule_doc[] = "Schedules a stream.";
static PyObject *
pyfsevents_schedule (PyObject *self,
                     PyObject *args)
{
    PyObject *thread = NULL;
    PyObject *stream = NULL;
    PyObject *paths = NULL;
    PyObject *callback = NULL;
    CFMutableArrayRef cf_array_paths = NULL;
    int i = 0;
    Py_ssize_t paths_size = 0;
    const char *path = NULL;
    CFStringRef cf_string_path = NULL;
    FSEventStreamInfo *stream_info = NULL;
    FSEventStreamRef fs_stream = NULL;
    CFRunLoopRef loop = NULL;

    if (!PyArg_ParseTuple(args, "OOOO:schedule", &thread, &stream, &callback, &paths))
        {
            return NULL;
        }

    /* Stream must not already be scheduled. */
    if (PyDict_Contains(g__pydict_streams, stream) == 1)
        {
            return NULL;
        }

    /* Create a paths array. */
    cf_array_paths = CFArrayCreateMutable(kCFAllocatorDefault, 1,
                                          &kCFTypeArrayCallBacks);
    if (cf_array_paths == NULL)
        {
            return NULL;
        }
    paths_size = PyList_Size(paths);
    for (i = 0; i < paths_size; ++i)
        {
            path = PyString_AS_STRING(PyList_GetItem(paths, i));
            cf_string_path = CFStringCreateWithCString(kCFAllocatorDefault,
                                                  path,
                                                  kCFStringEncodingUTF8);
            CFArraySetValueAtIndex(cf_array_paths, i, cf_string_path);
            CFRelease(cf_string_path);
        }

    /* Allocate stream information structure. */
    stream_info = PyMem_New(FSEventStreamInfo, 1);

    /* Create event stream. */
    FSEventStreamContext fs_stream_context = {0, (void*) stream_info, NULL, NULL, NULL};
    fs_stream = FSEventStreamCreate(kCFAllocatorDefault,
                                    (FSEventStreamCallback) &event_stream_handler,
                                    &fs_stream_context,
                                    cf_array_paths,
                                    kFSEventStreamEventIdSinceNow,
                                    0.01, // latency
                                    kFSEventStreamCreateFlagNoDefer);
    CFRelease(cf_array_paths);

    PyObject *value = NULL;
    value = PyCObject_FromVoidPtr((void *) fs_stream, PyMem_Free);
    PyDict_SetItem(g__pydict_streams, stream, value);

    /* Get runloop reference from observer info data or current. */
    value = PyDict_GetItem(g__pydict_loops, thread);
    if (value == NULL) {
        loop = CFRunLoopGetCurrent();
    } else {
        loop = (CFRunLoopRef) PyCObject_AsVoidPtr(value);
    }

    FSEventStreamScheduleWithRunLoop(fs_stream, loop, kCFRunLoopDefaultMode);

    /* Set stream info for callback. */
    stream_info->callback_event_handler = callback;
    stream_info->stream = fs_stream;
    stream_info->loop = loop;
    stream_info->thread_state = PyThreadState_Get();
    Py_INCREF(callback);

    /* Start event streams. */
    if (!FSEventStreamStart(fs_stream)) {
        FSEventStreamInvalidate(fs_stream);
        FSEventStreamRelease(fs_stream);
        return NULL;
    }

    Py_INCREF(Py_None);
    return Py_None;
}


/**
 * Unschedules a stream.
 *
 * @param self
 *     Python 'self'.
 * @param stream
 *     Stream to unschedule
 *
 * @return None
 */
static char pyfsevents_unschedule_doc[] = "Unschedules a stream.";
static PyObject *
pyfsevents_unschedule(PyObject *self,
                      PyObject *stream)
{
    PyObject *value = PyDict_GetItem(g__pydict_streams, stream);
    FSEventStreamRef fs_stream = PyCObject_AsVoidPtr(value);

    PyDict_DelItem(g__pydict_streams, stream);

    FSEventStreamStop(fs_stream);
    FSEventStreamInvalidate(fs_stream);
    FSEventStreamRelease(fs_stream);

    Py_INCREF(Py_None);
    return Py_None;
}


/**
 * Stops running the event loop in the specified thread.
 *
 * @param self
 *     Python 'self'.
 * @param thread
 *     Thread running the event runloop.
 *
 * @return None
 */
static char pyfsevents_stop_doc[] = "Stops running the event loop in the specified thread.";
static PyObject *
pyfsevents_stop(PyObject *self,
                PyObject *thread)
{
    PyObject *value = PyDict_GetItem(g__pydict_loops, thread);
    CFRunLoopRef loop = PyCObject_AsVoidPtr(value);

    /* Stop runloop */
    if (loop)
        {
            CFRunLoopStop(loop);
        }

    Py_INCREF(Py_None);
    return Py_None;
}


/**
 * Module public API.
 */
static PyMethodDef _fseventsmethods[] = {
    {"loop", pyfsevents_loop, METH_VARARGS, pyfsevents_loop_doc},
    {"stop", pyfsevents_stop, METH_O, pyfsevents_stop_doc},
    {"schedule", pyfsevents_schedule, METH_VARARGS, pyfsevents_schedule_doc},
    {"unschedule", pyfsevents_unschedule, METH_O, pyfsevents_unschedule_doc},
    {NULL, NULL, 0, NULL},
};


/**
 * Initialize the _fsevents module.
 */
#if PY_MAJOR_VERSION < 3
void
init_fsevents(void){
    PyObject *module = Py_InitModule3(MODULE_NAME, _fseventsmethods, MODULE_DOCUMENTATION);
    PyModule_AddIntConstant(module, MODULE_CONSTANT_NAME_POLLIN, kCFFileDescriptorReadCallBack);
    PyModule_AddIntConstant(module, MODULE_CONSTANT_NAME_POLLOUT, kCFFileDescriptorWriteCallBack);

    g__pydict_loops = PyDict_New();
    g__pydict_streams = PyDict_New();
}
#else /* PY_MAJOR_VERSION >= 3 */
static struct PyModuleDef _fseventsmodule = {
    PyModuleDef_HEAD_INIT,
    MODULE_NAME,
    MODULE_DOCUMENTATION,
    -1,
    _fseventsmethods
};
PyMODINIT_FUNC
PyInit__fsevents(void)
{
    PyObject *module = PyModule_Create(&_fseventsmodule);
    PyModule_AddIntConstant(module, MODULE_CONSTANT_NAME_POLLIN, kCFFileDescriptorReadCallBack);
    PyModule_AddIntConstant(module, MODULE_CONSTANT_NAME_POLLOUT, kCFFileDescriptorWriteCallBack);

    g__pydict_loops = PyDict_New();
    g__pydict_streams = PyDict_New();

    return module;
}
#endif /* PY_MAJOR_VERSION >= 3 */