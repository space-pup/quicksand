/*  quicksand/_quicksand.c – thin C wrapper for libquicksand.so
 *
 *  Purpose
 *  --------
 *  • Load the symbols from libquicksand.so at import time
 *  • Expose those functions to Python with proper argument/return
 *    conversion
 *  • Keep the GIL released while the library runs
 *
 *  Author:  Alec Graves
 *  Licence: AGPLv3
 */

#define _DARWIN_SOURCE
#define _GNU_SOURCE
#define PY_SSIZE_T_CLEAN

#include <Python.h>
#include <dlfcn.h>
#include <inttypes.h> /* for PRIu64 */
#include <stddef.h>
#include <stdint.h>

#include <stdio.h>

#include "quicksand.h" /* good for struct definitions */

/* ---------------- 1.  symbol pointers --------------------------------------*/

static void *lib_handle = NULL;

static int64_t (*p_quicksand_connect)(quicksand_connection **,
				      char *, int64_t, int64_t,
				      int64_t, void *) = NULL;
static void (*p_quicksand_disconnect)(quicksand_connection **,
				      void *) = NULL;
static void (*p_quicksand_delete)(char *, int64_t) = NULL;
static int64_t (*p_quicksand_write)(quicksand_connection *,
				    uint8_t *, int64_t) = NULL;
static int64_t (*p_quicksand_read)(quicksand_connection *,
				   uint8_t *, int64_t *) = NULL;
static uint64_t (*p_quicksand_now)(void) = NULL;
static double (*p_quicksand_ns)(uint64_t, uint64_t) = NULL;
static void (*p_quicksand_ns_calibrate)(double) = NULL;
static void (*p_quicksand_sleep)(double) = NULL;

/* ------------------------ 2.  load a single symbol -------------------------*/

static int
load_symbol(const char *sym, void **ptr)
{
	void *addr = dlsym(lib_handle, sym);
	if(!addr) {
		PyErr_Format(PyExc_RuntimeError,
			     "Failed to load symbol %s from libquicksand.so", sym);
		return -1;
	}
	*ptr = addr;
	return 0;
}

/* ----------- 3.  Create a PyCapsule holding a quicksand_connection* --------*/

static PyObject *
capsule_from_conn(quicksand_connection *conn)
{
	/* No destructor – we explicitly disconnect from Python side. */
	return PyCapsule_New((void *) conn,
			     "quicksand_connection",
			     NULL);
}

/* -------------------- 4.  Python exposed API -------------------------------*/

/* 4.  Connect – correct kwlist and format string */
static PyObject *
py_quicksand_connect(PyObject *self, PyObject *args, PyObject *kwds)
{
	static const char *kwlist[] = {"topic",
				       "message_size",
				       "message_rate",
				       NULL};

	const char *topic;
	Py_ssize_t topic_len = -1; /* will stay -1 (NUL‑terminated) */
	int64_t msg_sz = -1,
		msg_rate = -1;
	quicksand_connection *conn = NULL;
	int64_t rc;

	if(!PyArg_ParseTupleAndKeywords(args, kwds,
					"s|LL", kwlist,
					&topic, &msg_sz, &msg_rate)) {
		return NULL;
	}

	// printf("topic: %s\n", topic);
	// printf("msg_sz: %ld\n", msg_sz);
	// printf("msg_rate: %ld\n", msg_rate);

	rc = p_quicksand_connect(&conn,
				 (char *) topic,
				 (int64_t) topic_len,
				 msg_sz,
				 msg_rate,
				 NULL); /* alloc = NULL => stdlib malloc */

	if(rc < 0) {
		PyErr_Format(PyExc_RuntimeError,
			     "quicksand_connect failed with code %lld",
			     (long long) rc);
		return NULL;
	}

	return capsule_from_conn(conn);
}

static PyObject *
py_quicksand_delete(PyObject *self, PyObject *args, PyObject *kwds)
{
	static const char *kwlist[] = {"topic", NULL};

	const char *topic;
	Py_ssize_t topic_len = -1; /* will stay -1 (NUL‑terminated) */

	if(!PyArg_ParseTupleAndKeywords(args, kwds,
					"s|", kwlist,
					&topic)) {
		return NULL;
	}

	p_quicksand_delete((char *) topic,
			   (int64_t) topic_len);

	Py_RETURN_NONE;
}

static PyObject *
py_quicksand_disconnect(PyObject *self, PyObject *arg)
{
	quicksand_connection *conn;

	if(!PyCapsule_CheckExact(arg)) {
		PyErr_SetString(PyExc_TypeError,
				"expected a quicksand_connection capsule");
		return NULL;
	}

	conn = (quicksand_connection *) PyCapsule_GetPointer(arg,
							     "quicksand_connection");
	if(!conn) {
		return NULL; /* error already set */
	}

	p_quicksand_disconnect(&conn, NULL);
	Py_RETURN_NONE;
}

static PyObject *
py_quicksand_write(PyObject *self, PyObject *args)
{
	PyObject *capsule, *msg_obj;
	quicksand_connection *conn;
	Py_buffer view;
	int64_t rc;

	if(!PyArg_ParseTuple(args, "OO", &capsule, &msg_obj)) {
		return NULL;
	}

	if(!PyCapsule_CheckExact(capsule)) {
		PyErr_Format(PyExc_TypeError,
			     "first argument must be a quicksand_connection capsule");
	}

	conn = (quicksand_connection *) PyCapsule_GetPointer(capsule,
							     "quicksand_connection");
	if(!conn) {
		return NULL;
	}

	if(PyObject_GetBuffer(msg_obj, &view, PyBUF_SIMPLE) < 0) {
		return NULL;
	}

	/* Release the GIL – the C call can block */
	rc = p_quicksand_write(conn,
			       (uint8_t *) view.buf,
			       (int64_t) view.len);

	PyBuffer_Release(&view);

	if(rc < 0) {
		PyErr_Format(PyExc_RuntimeError,
			     "quicksand_write failed with code %lld",
			     (long long) rc);
	} else {
		Py_RETURN_NONE;
	}
	return NULL;
}

static PyObject *
py_quicksand_read(PyObject *self, PyObject *args)
{
	PyObject *capsule, *buf_obj;
	quicksand_connection *conn;
	Py_buffer view;
	int64_t msg_sz = 0, rc;
	int64_t remaining;

	if(!PyArg_ParseTuple(args, "OO", &capsule, &buf_obj)) {
		return NULL;
	}

	if(!PyCapsule_CheckExact(capsule)) {
		PyErr_Format(PyExc_TypeError,
			     "first argument must be a quicksand_connection capsule");
	}

	conn = (quicksand_connection *) PyCapsule_GetPointer(capsule,
							     "quicksand_connection");
	if(!conn) {
		return NULL;
	}

	/* Request a mutable view – the caller must supply something writable */
	if(PyObject_GetBuffer(buf_obj, &view,
			      PyBUF_WRITABLE | PyBUF_SIMPLE)
	   < 0) {
		return NULL;
	}

	/* QuickSand expects the max size we are willing to accept.  The
       value inside view.len is a Py_ssize_t which we cast to int64_t.
       After the call, `msg_sz` contains the actual number of bytes
       written into the buffer. */
	msg_sz = (int64_t) view.len;

	rc = p_quicksand_read(conn,
			      (uint8_t *) view.buf,
			      &msg_sz);

	PyBuffer_Release(&view);

	/* The library uses rc to indicate:
         rc > 0  – remaining messages in the ring.
         rc == -1 – no message was available at the moment
         rc < -1  – actual error code  */
	if(rc < 0) {
		if(rc == -1) {
			Py_RETURN_NONE;
		} else {
			PyErr_Format(PyExc_RuntimeError,
				     "quicksand_read failed with code %lld",
				     (long long) rc);
			return NULL;
		}
	}
	remaining = rc; /* guaranteed >0 here */

	/* `msg_sz` now contains how many bytes actually surface.  Slice the
       caller's buffer into a new `bytearray` that contains exactly those
       bytes.  We purposely avoid copying the full buffer – only the
       consumable part. */
	PyObject *payload = PyByteArray_FromStringAndSize(
			(char *) view.buf,
			(Py_ssize_t) msg_sz);
	if(!payload) {
		return NULL;
	}

	/* Return a 2‑tuple (payload, remaining). */
	return Py_BuildValue("Ni", payload, (int) remaining);
}

/* ------------------------------ misc helpers -------------------------------*/

static PyObject *
py_quicksand_remaining(PyObject *self, PyObject *args)
{
	PyObject *capsule;
	quicksand_connection *conn;
	Py_buffer view;
	int64_t msg_sz = 0, rc;
	int64_t remaining;

	if(!PyArg_ParseTuple(args, "O", &capsule)) {
		return NULL;
	}

	if(!PyCapsule_CheckExact(capsule)) {
		PyErr_Format(PyExc_TypeError,
			     "first argument must be a quicksand_connection capsule");
	}

	conn = (quicksand_connection *) PyCapsule_GetPointer(capsule,
							     "quicksand_connection");
	if(!conn) {
		return NULL;
	}
	return PyLong_FromUnsignedLongLong(conn->read_index - conn->buffer->index);
}

/* ------------------------------ timing functions -------------------------------*/

static PyObject *
py_quicksand_now(PyObject *self, PyObject *Py_UNUSED(ignored))
{
	return PyLong_FromUnsignedLongLong(p_quicksand_now());
}


static PyObject *
py_quicksand_ns(PyObject *self, PyObject *args)
{
	uint64_t end, start;

	if(!PyArg_ParseTuple(args, "KK", &end, &start)) {
		return NULL;
	}
	return PyFloat_FromDouble(p_quicksand_ns(end, start));
}

static PyObject *
py_quicksand_sleep(PyObject *self, PyObject *args)
{
	double ns;

	if(!PyArg_ParseTuple(args, "d", &ns)) {
		return NULL;
	}
	Py_BEGIN_ALLOW_THREADS
	p_quicksand_sleep(ns);
	Py_END_ALLOW_THREADS
	Py_RETURN_NONE;
}

/* --------------------5.  Module method table -------------------------------*/

static PyMethodDef QuickSandMethods[] = {
		{"connect", py_quicksand_connect,
		 METH_VARARGS | METH_KEYWORDS,
		 "Connect to a quicksand ring buffer and return a capsule."},
		{"disconnect", py_quicksand_disconnect,
		 METH_O, "Disconnect and free a connection capsule."},
		{"delete", py_quicksand_delete,
		 METH_VARARGS | METH_KEYWORDS, "Remove a shared memory buffer for future connections."},
		{"write", py_quicksand_write,
		 METH_VARARGS, "Write a bytes‑like object to the ring buffer."},
		{"read", py_quicksand_read,
		 METH_VARARGS, "Read a message into a mutable buffer, returning (msg, remaining)."},
		{"now", py_quicksand_now, METH_NOARGS,
		 "Monotonic timestamp (raw cycles)."},
		{"ns", py_quicksand_ns, METH_VARARGS,
		 "Nanoseconds between two timestamps."},
		{"sleep", py_quicksand_sleep, METH_VARARGS,
		 "Sleep for the given number of nanoseconds."},
		{NULL, NULL, 0, NULL}};

/* -------------------------- 6.  Module definition --------------------------*/

static struct PyModuleDef quicksandmodule = {
		PyModuleDef_HEAD_INIT,
		"quicksand._quicksand",		      /* full name */
		"Thin C wrapper for libquicksand.so", /* doc */
		-1,				      /* per‑interpreter state */
		QuickSandMethods,
		NULL, NULL, NULL, NULL};

/* ----------------------------- 7.  Module init -----------------------------*/

PyMODINIT_FUNC
PyInit__quicksand(void)
{
	PyObject *m = PyModule_Create(&quicksandmodule);
	if(!m) {
		return NULL;
	}

	/* Resolve the shared library as a sibling of the wrapper */
	/* Build the runtime path:  <pkg>/quicksand/_quicksand.so -> <pkg>/libquicksand.so */
	/* We compute it relative to the directory of the built module. */
	PyObject *module_path = PyUnicode_FromString(__FILE__);
	/* __FILE__ points at /path/to/quicksand/_quicksand.c – we need "/"->"../libquicksand.so";
       but the relative path here is sufficient for a typical build layout. */
	const char *so_path = "libquicksand.so";
	lib_handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
	if(!lib_handle) {
		PyErr_SetString(PyExc_RuntimeError,
				"Failed to load shared library libquicksand.so");
		return NULL;
	}

	/* Resolve all required symbols and abort if any is missing. */
	if(load_symbol("quicksand_connect", (void **) &p_quicksand_connect) < 0) {
		return NULL;
	}
	if(load_symbol("quicksand_disconnect", (void **) &p_quicksand_disconnect) < 0) {
		return NULL;
	}
	if(load_symbol("quicksand_delete", (void **) &p_quicksand_delete) < 0) {
		return NULL;
	}
	if(load_symbol("quicksand_delete", (void **) &p_quicksand_delete) < 0) {
		return NULL;
	}
	if(load_symbol("quicksand_write", (void **) &p_quicksand_write) < 0) {
		return NULL;
	}
	if(load_symbol("quicksand_read", (void **) &p_quicksand_read) < 0) {
		return NULL;
	}
	if(load_symbol("quicksand_now", (void **) &p_quicksand_now) < 0) {
		return NULL;
	}
	if(load_symbol("quicksand_ns", (void **) &p_quicksand_ns) < 0) {
		return NULL;
	}
	if(load_symbol("quicksand_ns_calibrate", (void **) &p_quicksand_ns_calibrate) < 0) {
		return NULL;
	}
	if(load_symbol("quicksand_sleep", (void **) &p_quicksand_sleep) < 0) {
		return NULL;
	}

	return m;
}
