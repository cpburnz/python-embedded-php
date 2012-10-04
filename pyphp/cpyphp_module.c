/**
PyPHP is a python module written in C that embeds the PHP interpreter.

:Authors: Caleb P. Burns <cpburnz@gmail.com>; Ben DeMott <ben_demott@hotmail.com>
:Version: 0.5
:Status: Development
:Date: 2012-06-11 

.. TODO: IMPLEMENT THIS TONIGHT. Both zend_execute_scripts() and
   zend_eval_stringl() accept an argument for getting the return value.

.. TODO: In the next verion (0.6?) release python GIL while PHP API calls are
   being made. ``PyEval_SaveThread()`` can be used to save the thread state
   and release the GIL. The GIL can be re-acquired and thread state restored
   with ``PyEval_RestoreThread()``. By intermixing these calls the GIL can be
   released right before a PHP API call starts, acquired when Python code is
   encountered during the call (i.e., output, logging, errors, etc.), released
   when returning to PHP, and finally re-acquired after the end of the PHP
   call.
*/

// TODO: Where are FAILURE and SUCCESS defined?

// Use Py_ssize_t instead of int in Python APIs.
#define PY_SSIZE_T_CLEAN

#include <Python.h> // Py*, PY*

#include <limits.h> // INT_MAX
#include <stdarg.h> // va_list
#include <stdbool.h> // bool, false, true
#include <stddef.h> // NULL
#include <stdio.h> // FILE, fdopen, fflush, fopen, fputc, fputs, stdout

#include <sapi/embed/php_embed.h> // sapi_module_struct, php*
#include <main/spprintf.h> // spprintf, vspprintf
#include <Zend/zend_API.h> // array_init, array_init_size
#include <Zend/zend_globals_macros.h> // EG
#include <Zend/zend_hash.h> // zend_hash_*, zend_symtable_*
#include <Zend/zend_errors.h> // E_*
#include <Zend/zend_ini.h> // zend_alter_ini_entry, zend_ini_*
#include <Zend/zend_modules.h> // zend_module_entry

#include "cpyphp_zval.inl.c" // zval_copy, zval_del, zval_from_*, zval_is_list, zval_to_*

// Shorten print format macros.
#define PY_Z PY_FORMAT_SIZE_T

static struct pyphp_t {
	bool is_inited;
	bool is_started;
	
	// Output file pointers.
	FILE * err_fp;
	FILE * log_fp;
	FILE * out_fp;
	
	// Python callback functions.
	PyObject * pyerr_cb;
	PyObject * pylog_cb;
	PyObject * pyout_cb;
	
	// Python thread state.
	// .. NOTE: This not implemented.
	//PyThreadSafe * pysave;
	
	// Interal PHP error handler.
	void (* php_internal_error_cb)(int type, const char * file, const unsigned int line, const char * format, va_list args) ZEND_ATTRIBUTE_PTR_FORMAT(printf, 4, 0);
} pyphp;


/**************************** Python Exceptions *****************************/

static const char * PyphpExceptionType_doc = (
	"The ``PyphpException`` class is the base exception that all custom PyPHP\n"
	"exceptions will be derived from."
);

static PyObject * PyphpExceptionType = NULL;

static const char * InternalErrorType_doc = (
	"The ``InternalError`` exception is raised when there is an internal\n"
	"error within PyPHP that cannot be resolved."
);

static PyObject * InternalErrorType = NULL;

static const char * PhpFatalErrorType_doc = (
	"The ``PhpFatalError`` exception is raised when PHP encounters a fatal\n"
	"error."
);

static PyObject * PhpFatalErrorType = NULL;



/***************************** Utility Methods ******************************/

/**
Converts a Python value to a PHP value.

*pyobj* (``PyObject *``) is the python value.

.. NOTE: If this is a ``PyUnicodeObject``, the resulting PHP value will
   contain a UTF-8 encoded string.

*pymemo* (``PyObject *``) is a memo dictionary of objects already copied. This
is used internally by this method so it should be set to ``NULL``.

Returns the new PHP value (``zval *``).

.. NOTE: If the return value is ``NULL``, a Python exception has been raised.
*/
static zval * PyObject_to_zval(PyObject * pyobj, PyObject * pymemo) {
	if (pyobj == NULL) {
		PyErr_Format(InternalErrorType, "Python object:%p is NULL.", (void *)pyobj);
		return NULL;
		
	} else if (pyobj == Py_None) {
		zval * zv = NULL; // owned
		// Convert python none to php null.
		zv = zval_from_null();
		if (zv == NULL) {
			PyObject * repr = PyObject_Repr(pyobj);
			if (repr != NULL) {
				PyErr_Format(InternalErrorType, "Failed to convert Python object:%s to PHP null.", PyString_AS_STRING(repr));
				Py_DECREF(repr);
			}
		}
		return zv;
		
	} else if (PyBool_Check(pyobj)) {
		zval * zv = NULL; // owned
		// Convert python bool to php bool.
		zv = zval_from_bool(PyInt_AS_LONG(pyobj) ? true : false);
		if (zv == NULL) {
			PyObject * repr = PyObject_Repr(pyobj);
			if (repr != NULL) {
				PyErr_Format(InternalErrorType, "Failed to convert Python object:%s to PHP bool.", PyString_AS_STRING(repr));
				Py_DECREF(repr);
			}
		}
		return zv;
		
	} else if (PyInt_Check(pyobj)) {
		zval * zv = NULL; // owned
		// Convert python int to php long.
		zv = zval_from_long(PyInt_AS_LONG(pyobj));
		if (zv == NULL) {
			PyObject * repr = PyObject_Repr(pyobj);
			if (repr != NULL) {
				PyErr_Format(InternalErrorType, "Failed to convert Python object:%s to PHP long.", PyString_AS_STRING(repr));
				Py_DECREF(repr);
			}
		}
		return zv;
		
	} else if (PyLong_Check(pyobj)) {
		zval * zv = NULL; // owned
		long result;
		// Convert python long to php long.
		result = PyLong_AsLong(pyobj);
		if (PyErr_Occurred() == NULL) {
			zv = zval_from_long(result);
			if (zv == NULL) {
				PyObject * repr = PyObject_Repr(pyobj);
				if (repr != NULL) {
					PyErr_Format(InternalErrorType, "Failed to convert Python object:%s to PHP long.", PyString_AS_STRING(repr));
					Py_DECREF(repr);
				}
			}
		}
		return zv;
		
	} else if (PyFloat_Check(pyobj)) {
		zval * zv = NULL; // owned
		// Convert python float to php double.
		zv = zval_from_double(PyFloat_AS_DOUBLE(pyobj));
		if (zv == NULL) {
			PyObject * repr = PyObject_Repr(pyobj);
			if (repr != NULL) {
				PyErr_Format(InternalErrorType, "Failed to convert Python object:%s to PHP double.", PyString_AS_STRING(repr));
				Py_DECREF(repr);
			}
		}
		return zv;
		
	} else if (PyString_Check(pyobj)) {
		zval * zv = NULL; // owned
		Py_ssize_t pylen;
		// Convert python string to php string.
		pylen = PyString_GET_SIZE(pyobj);
		if (pylen < 0 || INT_MAX < pylen) {
			PyErr_Format(PyExc_ValueError, "Python object:%s length:%" PY_Z "i must be between 0 and %i.", Py_TYPE(pyobj)->tp_name, pylen, INT_MAX);
			return NULL;
		}
		zv = zval_from_string(PyString_AS_STRING(pyobj), (int)pylen);
		if (zv == NULL) {
			PyObject * repr = PyObject_Repr(pyobj);
			if (repr != NULL) {
				PyErr_Format(InternalErrorType, "Failed to convert Python object:%s to PHP string.", PyString_AS_STRING(repr));
				Py_DECREF(repr);
			}
		}
		return zv;
		
	} else if (PyUnicode_Check(pyobj)) {
		zval * zv = NULL; // owned
		PyObject * pystr = NULL; // owned
		Py_ssize_t pylen;
		// Convert python unicode string to php string encoded as utf-8.
		pystr = PyUnicode_AsUTF8String(pyobj);
		if (pystr != NULL) {
			pylen = PyString_GET_SIZE(pystr);
			if (pylen < 0 || INT_MAX < pylen) {
				PyErr_Format(PyExc_ValueError, "Python object:%s length:%" PY_Z "i must be between 0 and %i.", Py_TYPE(pyobj)->tp_name, pylen, INT_MAX);
			} else {
				zv = zval_from_string(PyString_AS_STRING(pystr), (int)pylen);
				if (zv == NULL) {
					PyObject * repr = PyObject_Repr(pyobj);
					if (repr != NULL) {
						PyErr_Format(InternalErrorType, "Failed to convert Python object:%s to PHP string.", PyString_AS_STRING(repr));
						Py_DECREF(repr);
					}
				}
			}
			Py_DECREF(pystr);
		}
		return zv;
		
	} else if (PyDict_Check(pyobj)) {
		bool pymemo_is_tmp = false;
		Py_ssize_t pylen = 0;
		Py_ssize_t pypos = 0;
		Py_ssize_t keylen = 0;
		const char * key = NULL; // borrowed
		zval * zdict = NULL; // owned
		zval * zv = NULL; // owned
		PyObject * pyptr = NULL; // borrowed
		PyObject * pykey = NULL; // borrowed
		PyObject * pyval = NULL; // borrowed
		PyObject * pyptr_tmp = NULL; // owned
		PyObject * pykey_tmp = NULL; // owned
		PyObject * pyval_tmp = NULL; // owned
	
		// Make sure the dict is small enough.
		pylen = PyDict_Size(pyobj);
		if (pylen < 0 || INT_MAX < pylen) {
			PyErr_Format(PyExc_ValueError, "Python object:%s length:%" PY_Z "i must be between 0 and %i inclusive.", Py_TYPE(pyobj)->tp_name, pylen, INT_MAX);
			return NULL;
		}
	
		// Initialize PHP array.
		MAKE_STD_ZVAL(zdict);
		if (zdict == NULL) {
			PyErr_Format(InternalErrorType, "Failed to create zval.");
			return NULL;
		}
		if (array_init(zdict) != SUCCESS) {
			PyErr_Format(InternalErrorType, "Failed to initialize zval array.");
			goto dict_error;
		}
		
		// Create memo dict if we don't have one.
		if (pymemo == NULL) {
			pymemo = PyDict_New();
			if (pymemo == NULL) {
				goto dict_error;
			}
			pymemo_is_tmp = true;
		}
		
		// Iterate over python dict, convert python key-value pairs into PHP key
		// value pairs and append them to the PHP array.
		while (PyDict_Next(pyobj, &pypos, &pykey, &pyval)) {
		
			// Get python key string.
			if (pykey == Py_None || pykey == Py_False) {
				// In PHP both `null` and `false` become empty strings when casted.
				key = "";
				keylen = 0;
			} else {
				if (PyString_Check(pykey)) {
					key = PyString_AS_STRING(pykey);
					keylen = PyString_GET_SIZE(pykey);
				} else {
					if (PyUnicode_Check(pykey)) {
						pykey_tmp = PyUnicode_AsUTF8String(pykey);
					} else {
						pykey_tmp = PyObject_Str(pykey);
					}
					if (pykey_tmp == NULL) {
						goto dict_error; // Clean up.
					}
					key = PyString_AS_STRING(pykey_tmp);
					keylen = PyString_GET_SIZE(pykey_tmp);
				}
				if (keylen < 0 || INT_MAX < keylen) {
					PyErr_Format(InternalErrorType, "Python key:%s length:%" PY_Z "i must be between 0 and %i inclusive.", Py_TYPE(pykey)->tp_name, keylen, INT_MAX);
					goto dict_error; // Clean up.
				}
			}
			
			// Convert python value to php value.
			// - The memo dict maps python value pointer to php value pointer.
			pyval_tmp = PyLong_FromVoidPtr(pyval);
			if (pyval_tmp == NULL) {
				goto dict_error; // Clean up.
			}
			pyptr = PyDict_GetItem(pymemo, pyval_tmp);
			if (pyptr != NULL) {
				// Get mapped php value pointer from python value pointer.
				zv = PyLong_AsVoidPtr(pyptr);
				if (zv == NULL) {
					goto dict_error; // Clean up.
				}
			} else {
				zv = PyObject_to_zval(pyval, pymemo);
				if (zv == NULL) {
					goto dict_error; // Clean up.
				}
				// Map python value pointer to php value pointer to support recursion.
				pyptr_tmp = PyLong_FromVoidPtr(zv);
				if (pyptr_tmp == NULL) {
					goto dict_error; // Clean up.
				}
				if (PyDict_SetItem(pymemo, pyval_tmp, pyptr_tmp) != 0) {
					goto dict_error; // Clean up.
				}
			}
			
			// Set new php value in array.
			// .. NOTE: Hash key length MUST include NULL byte.
			if (zend_symtable_update(Z_ARRVAL_P(zdict), key, (unsigned int)keylen + 1, &zv, sizeof(zv), NULL) != SUCCESS) {
				PyErr_Format(InternalErrorType, "Failed to set key in php array.");
				goto dict_error;
			}
			zv = NULL; // PHP dict steals reference to php value.
			
			// Destroy temporary python values.
			if (pyptr_tmp != NULL) {
				Py_DECREF(pyptr_tmp);
				pyptr_tmp = NULL;
			}
			if (pyval_tmp != NULL) {
				Py_DECREF(pyval_tmp);
				pyval_tmp = NULL;
			}
			if (pykey_tmp != NULL) {
				Py_DECREF(pykey_tmp);
				pykey_tmp = NULL;
			}
		}
		
		// Clean up remaining python temporary values.
		if (pymemo_is_tmp) {
			Py_DECREF(pymemo);
		}
		
		// Return new PHP array.
		return zdict;
		
		// Failed to convert python dict to php array.
		dict_error: {
			// Clean up temporary python values.
			if (pyptr_tmp != NULL) {
				Py_DECREF(pyptr_tmp);
			}
			if (pyval_tmp != NULL) {
				Py_DECREF(pyval_tmp);
			}
			if (pykey_tmp != NULL) {
				Py_DECREF(pykey_tmp);
			}
			if (pymemo_is_tmp) {
				Py_DECREF(pymemo);
			}
			// Destory orphaned php value.
			if (zv != NULL) {
				zval_del(&zv);
			}
			// Destory php array.
			zval_del(&zdict);
		}
		return NULL;
		
	} else if (PySequence_Check(pyobj)) {
		bool pymemo_is_tmp = false;
		Py_ssize_t i = 0;
		Py_ssize_t pylen = 0;
		zval * zlist = NULL; // owned
		zval * zv = NULL; // owned
		PyObject * pyfast = NULL; // owned
		PyObject * pyptr = NULL; // borrowed
		PyObject * pyval = NULL; // borrowed
		PyObject * pyptr_tmp = NULL; // owned
		PyObject * pyval_tmp = NULL; // owned
		PyObject ** pyitems = NULL; // borrowed
		
		// Make sure sequence is either a tuple or list.
		pyfast = PySequence_Fast(pyobj, "");
		if (pyfast == NULL) {
			// Override exception.
			PyObject * repr = PyObject_Repr(pyobj);
			if (repr != NULL) {
				PyErr_Format(PyExc_TypeError, "Failed to convert Python object:%s to list.", PyString_AS_STRING(repr));
				Py_DECREF(repr);
			}
			return NULL;
		}
		
		// Make sure the sequence is small enough
		pylen = PySequence_Fast_GET_SIZE(pyfast);
		if (pylen < 0 || INT_MAX < pylen) {
			PyErr_Format(PyExc_ValueError, "Python object:%s length:%" PY_Z "i must be between 0 and %i inclusive.", Py_TYPE(pyobj)->tp_name, pylen, INT_MAX);
			goto list_error;
		}
		
		// Initialize PHP array.
		MAKE_STD_ZVAL(zlist);
		if (zlist == NULL) {
			PyErr_Format(InternalErrorType, "Failed to create zval.");
			goto list_error;
		}
		if (array_init_size(zlist, (unsigned int)pylen) != SUCCESS) {
			PyErr_Format(InternalErrorType, "Failed to initialize zval array.");
			goto list_error;
		}
		
		// Create memo dict if we don't have one.
		if (pymemo == NULL) {
			pymemo = PyDict_New();
			if (pymemo == NULL) {
				goto list_error;
			}
			pymemo_is_tmp = true;
		}
		
		// Iterate over python sequence, convert python values into PHP key values
		// and append them to the PHP array.
		pyitems = PySequence_Fast_ITEMS(pyfast);
		for (i = 0; i < pylen; ++i) {
			// Convert python value to php value.
			// - The memo dict maps python value pointer to php value pointer.
			pyval = pyitems[i];
			pyval_tmp = PyLong_FromVoidPtr(pyval);
			if (pyval_tmp == NULL) {
				goto list_error; // Clean up.
			}
			pyptr = PyDict_GetItem(pymemo, pyval_tmp);
			if (pyptr != NULL) {
				// Get mapped php value pointer from python value pointer.
				zv = PyLong_AsVoidPtr(pyptr);
				if (zv == NULL) {
					goto list_error; // Clean up.
				}
			} else {
				zv = PyObject_to_zval(pyval, pymemo);
				if (zv == NULL) {
					goto list_error; // Clean up.
				}
				// Map python value pointer to php value pointer to support recursion.
				pyptr_tmp = PyLong_FromVoidPtr(zv);
				if (pyptr_tmp == NULL) {
					goto list_error; // Clean up.
				}
				if (PyDict_SetItem(pymemo, pyval_tmp, pyptr_tmp) != 0) {
					goto list_error; // Clean up.
				}
			}
			
			// Set new php value in array.
			if (zend_hash_next_index_insert(Z_ARRVAL_P(zlist), &zv, sizeof(zv), NULL) != SUCCESS) {
				PyErr_Format(InternalErrorType, "Failed to set index:%" PY_Z "i in php array.", i);
				goto list_error;
			}
			zv = NULL; // PHP array steals reference to php value.
			
			// Destroy temporary python values.
			if (pyptr_tmp != NULL) {
				Py_DECREF(pyptr_tmp);
				pyptr_tmp = NULL;
			}
			if (pyval_tmp != NULL) {
				Py_DECREF(pyval_tmp);
				pyval_tmp = NULL;
			}
		}
		
		// Clean up remaining python temporary values.
		if (pymemo_is_tmp) {
			Py_DECREF(pymemo);
		}
		Py_DECREF(pyfast);
		
		// Return new PHP array.
		return zlist;
		
		// Failed to convert python sequence to php array.
		list_error: {
			// Clean up temporary python values.
			if (pyptr_tmp != NULL) {
				Py_DECREF(pyptr_tmp);
			}
			if (pyval_tmp != NULL) {
				Py_DECREF(pyval_tmp);
			}
			if (pymemo_is_tmp) {
				Py_DECREF(pymemo);
			}
			// Destroy orphaned php value.
			if (zv != NULL) {
				zval_del(&zv);
			}
			// Destory php array.
			if (zlist != NULL) {
				zval_del(&zlist);
			}
			// Clean up sequence.
			Py_DECREF(pyfast);
		}
		return NULL;
	}
	
	// Python object not supported.
	PyErr_Format(PyExc_TypeError, "Python type:%s cannot be converted to a PHP value.", Py_TYPE(pyobj)->tp_name);
	return NULL;
}



/**
Converts a PHP value to a Python value.

*zobj* (``zval *``) is the php value.

*pymemo* (``PyObject *``) is a memo dictionary of objects already copied. This
is used internally by this method so it should be set to ``NULL``.

Returns the new Python value (``PyObject *``).

.. NOTE: If the return value is ``NULL``, a Python exception has been raised.
*/
static PyObject * zval_to_PyObject(zval * zobj, PyObject * pymemo) {
	if (zobj == NULL) {
		PyErr_Format(InternalErrorType, "PHP value:%p is NULL.", (void *)zobj);
		return NULL;
		
	}
	switch (Z_TYPE_P(zobj)) {
		case IS_NULL:
			Py_RETURN_NONE;
			
		case IS_LONG:
			return PyInt_FromLong(Z_LVAL_P(zobj));
			
		case IS_DOUBLE:
			return PyFloat_FromDouble(Z_DVAL_P(zobj));
			
		case IS_BOOL:
			if (Z_LVAL_P(zobj)) {
				Py_RETURN_TRUE;
			}
			Py_RETURN_FALSE;
			
		case IS_ARRAY:
		case IS_CONSTANT_ARRAY:
			if (zval_is_list(zobj)) {
				// Convert to list.
				bool pymemo_is_tmp = false;
				PyObject * pylist = NULL; // owned
				PyObject * pyval = NULL; // owned
				PyObject * pyzv = NULL; // owned
				Bucket * p = NULL; // borrowed
				zval * zv = NULL; // borrowed
				
				// Create python list.
				pylist = PyList_New(zend_hash_num_elements(Z_ARRVAL_P(zobj)));
				if (pylist == NULL) {
					return NULL;
				}
		
				// Create memo dict if we don't have one.
				if (pymemo == NULL) {
					pymemo = PyDict_New();
					if (pymemo == NULL) {
						goto list_error; // Clean up.
					}
					pymemo_is_tmp = true;
				}
				
				// Iterate over php list, convert php values into python values, and
				// append them to the python list.
				p = Z_ARRVAL_P(zobj)->pListHead;
				while (p != NULL) {
					// Convert php value to python value.
					// .. NOTE: The memo dict maps php value pointer to python value.
					zv = p->pData;
					pyzv = PyLong_FromVoidPtr(zv);
					if (pyzv == NULL) {
						goto list_error; // Clean up.
					}
					pyval = PyDict_GetItem(pymemo, pyzv);
					if (pyval != NULL) {
						Py_INCREF(pyval);
					} else {
						pyval = zval_to_PyObject(zv, pymemo);
						if (pyval == NULL) {
							goto list_error; // Clean up.
						}
						// Map php value pointer to python value to support recursion.
						if (PyDict_SetItem(pymemo, pyzv, pyval) != 0) {
							goto list_error; // Clean up.
						}
					}
					
					// Set new python value in list.
					// .. NOTE: Numeric indices stored in h, and zend_is_list() ensured
					//    that all keys were numeric and within 0 and 2**32-1 inclusive.
					PyList_SET_ITEM(pylist, (Py_ssize_t)p->h, pyval);
					pyval = NULL; // Python list steals reference to python value.
					
					// Clean-up temporary values.
					Py_DECREF(pyzv);
					pyzv = NULL;
					
					p = p->pListNext;
				}
				
				// Clean-up temporary values.
				if (pymemo_is_tmp) {
					Py_DECREF(pymemo);
				}
				
				// Return new python list.
				return pylist;
				
				// Failed to convert php list to python list.
				list_error: {
					// Clean-up temporary values.
					Py_XDECREF(pyval);
					Py_XDECREF(pyzv);
					if (pymemo_is_tmp) {
						Py_DECREF(pymemo);
					}
					// Destroy python list.
					Py_DECREF(pylist);
				}
				return NULL;
				
			} else {
				// Convert to dict.
				bool pymemo_is_tmp = false;
				PyObject * pydict = NULL; // owned
				PyObject * pykey = NULL; // owned
				PyObject * pyval = NULL; // owned
				PyObject * pyzv = NULL; // owned
				Bucket * p = NULL; // borrowed
				zval * zv = NULL; // borrowed
				
				// Create python dict.
				pydict = PyDict_New();
				if (pydict == NULL) {
					return NULL;
				}
				
				// Create memo dict if we don't have one.
				if (pymemo == NULL) {
					pymemo = PyDict_New();
					if (pymemo == NULL) {
						goto dict_error; // Clean up.
					}
					pymemo_is_tmp = true;
				}
				
				// Iterate over php dict, convert php keys and values into python keys
				// and values, and add them to the python dict.
				p = Z_ARRVAL_P(zobj)->pListHead;
				while (p != NULL) {
					// Convert php key to python key.
					if (p->nKeyLength > PY_SSIZE_T_MAX) {
						PyErr_Format(PyExc_ValueError, "PHP key length:%u is not between 0 and %" PY_Z "i inclusive.", p->nKeyLength, PY_SSIZE_T_MAX);
						goto dict_error; // Clean up.
					}
					pykey = PyString_FromStringAndSize(p->arKey, (Py_ssize_t)p->nKeyLength);
					if (pykey == NULL) {
						goto dict_error; // Clean up.
					}
					
					// Convert php value to python value.
					// .. NOTE: The memo dict maps php value pointer to python value.
					zv = p->pData;
					pyzv = PyLong_FromVoidPtr(zv);
					if (pyzv == NULL) {
						goto dict_error; // Clean up.
					}
					pyval = PyDict_GetItem(pymemo, pyzv);
					if (pyval != NULL) {
						Py_INCREF(pyval);
					} else {
						pyval = zval_to_PyObject(zv, pymemo);
						if (pyval == NULL) {
							goto dict_error; // Clean up.
						}
						// Map php value pointer to python value to support recursion.
						if (PyDict_SetItem(pymemo, pyzv, pyval) != 0) {
							goto dict_error; // Clean up.
						}
					}
					
					// Set new python value in dict.
					if (PyDict_SetItem(pydict, pykey, pyval) != 0) {
						goto dict_error; // Clean up.
					}
					
					// Clean-up temporary values.
					Py_DECREF(pykey);
					pykey = NULL;
					Py_DECREF(pyzv);
					pyzv = NULL;
					Py_DECREF(pyval);
					pyval = NULL;
					
					p = p->pListNext;
				}
				
				// Clean-up temporary values.
				if (pymemo_is_tmp) {
					Py_DECREF(pymemo);
				}
				
				// Return new python dict.
				return pydict;
				
				// Failed to convert php dict to python dict.
				dict_error: {
					// Clean-up temporary values.
					Py_XDECREF(pyval);
					Py_XDECREF(pyzv);
					Py_XDECREF(pykey);
					if (pymemo_is_tmp) {
						Py_DECREF(pymemo);
					}					
					// Destory python dict.
					Py_DECREF(pydict);
				}
				return NULL;
			}
			break;
			
		case IS_OBJECT:
			// .. TODO: Support converting an object to a dict.
			PyErr_Format(PyExc_NotImplementedError, "Converting PHP object type to Python dict is not yet implemented.");
			return NULL;
			
		case IS_STRING:
		case IS_CONSTANT:
			return PyString_FromStringAndSize(Z_STRVAL_P(zobj), Z_STRLEN_P(zobj));
			
		case IS_RESOURCE:
			// .. TODO: Return a PhpResource instance that extends int.
			return PyString_FromFormat("<Resource %li>", Z_RESVAL_P(zobj));
			
		default:
			break;
	}
	
	// PHP object not supported.
	PyErr_Format(PyExc_TypeError, "PHP type:%hhu cannot be converted to a Python value.", Z_TYPE_P(zobj));
	return NULL;
}



/******************************* PHP Methods ********************************/

static bool pyphp_php_restart();
static void pyphp_php_log_cb(char * message);
static int pyphp_php_output_cb(const char * str, unsigned int str_length TSRMLS_DC);
static void pyphp_php_output_flush_cb(void * server_ctx);
static int pyphp_php_startup_cb(sapi_module_struct * sapi);

/**
Executes the specified PHP script.

*name* (``const char *``) is the name of the script.

*name_len* (``Py_ssize_t``) is the length of name.

*fp* (``FILE *``) is the file pointer to the script.

.. NOTE: This reference is stolen.

Returns ``true`` on success; otherwise, ``false``.
*/
static bool pyphp_php_exec_file(const char * name, Py_ssize_t name_len, FILE * fp) {
	zend_file_handle zfile;
	bool result = false;

	// Make sure PyPHP has been started.
	if (!pyphp.is_started) {
		PyErr_SetString(InternalErrorType, "PyPHP not initialized.");
		return false;
	}
	
	if (name_len < 0 || INT_MAX < name_len) {
		PyErr_Format(PyExc_ValueError, "name_len:%" PY_Z "i must be between 0 and %i inclusive.", name_len, INT_MAX);
		return false;
	}
	
	// Setup zend file handle.
	zfile.type = ZEND_HANDLE_FP;
	zfile.filename = (char *)name; // NOTE: I think this cast is safe.
	zfile.opened_path = NULL;
	zfile.handle.fp = fp;
	zfile.free_filename = 0;
	
	// Execute script.
	// .. TODO: Properly send php errors to python.
	{
		TSRMLS_FETCH();
		result = true;
		zend_first_try {
			if (zend_execute_scripts(ZEND_REQUIRE TSRMLS_CC, NULL, 1, &zfile) != SUCCESS) {
				result = false;
			}
		} zend_catch {
			result = false;
		} zend_end_try();
	}
	
	// Reset php.
	if (!pyphp_php_restart()) {
		return false;
	}
	
	// Check for python exception.
	if (PyErr_Occurred() != NULL) {
		return false;
	} else if (!result) {
		PyErr_SetString(InternalErrorType, "Failed to execute script.");
		return false;
	}
	
	return true;
}

/**
Executes the specified PHP inline string/script.

*name* (``const char *``) is the name of the inline string/script.

*str* (``const char *``) is the string to execute.

*str_len* (``int``) is the length of *str*.
 
Returns ``true`` on success; otherwise, ``false``.
*/
static bool pyphp_php_exec_inline(const char * name, const char * str, int str_len) {
	bool result = false;

	// Make sure PyPHP has been started.
	if (!pyphp.is_started) {
		PyErr_SetString(InternalErrorType, "PyPHP not initialized.");
		return false;
	}
	
	// Execute string.
	// .. TODO: Properly send php errors to python.
	{
		TSRMLS_FETCH();
		result = true;
		zend_first_try {
			// NOTE: I think casting these to `char *` is safe.
			if (zend_eval_stringl((char *)str, str_len, NULL, (char *)name TSRMLS_CC) != SUCCESS) {
				result = false;
			}
		} zend_catch {
			result = false;
		} zend_end_try();
	}
	
	// Reset php.
	if (!pyphp_php_restart()) {
		return false;
	}
	
	// Check for python exception.
	if (PyErr_Occurred() != NULL) {
		return false;
	} else if (!result) {
		PyErr_SetString(InternalErrorType, "Failed to execute string.");
		return false;
	}
	
	return result;
}

/**
Gets the value of the specified global variable.

*key* (``const char *``) is the name of the variable to get.

*keylen* (``int``) is the length of *key*.

*var* (``const char *``) optionally gets the keyed value from the specified
global variable instead of from the global symbol table.

*varlen* (``int``) is the length of *var*.

Returns a borrowed reference to value (``zval *``) of the global variable.
*/
static zval * pyphp_php_global_get(const char * key, int keylen, const char * var, int varlen) {
	HashTable * ht = NULL; // borrowed
	zval * zv = NULL; // borrowed
	
	// Make sure PyPHP has been started.
	if (!pyphp.is_started) {
		PyErr_SetString(InternalErrorType, "PyPHP not initialized.");
		return NULL;
	}
	
	{
		TSRMLS_FETCH();
		if (var != NULL) {
			zval * zdict = NULL; // borrowed
			// Get hash table for specified var.
			// .. NOTE: Hash key length must include NULL byte.
			if (zend_symtable_find(&EG(symbol_table), var, (unsigned int)varlen + 1, &zdict) != SUCCESS) {
				PyErr_SetString(PyExc_KeyError, "var is not set.");
				return NULL;
			}
			ht = Z_ARRVAL_P(zdict);
		} else {
			// Get global symbol table.
			ht = &EG(symbol_table);
		}
	}
	
	// Get php variable.
	// .. NOTE: Hash key length MUST include NULL byte.
	if (zend_symtable_find(ht, key, (unsigned int)keylen + 1, &zv) != SUCCESS) {
		PyErr_SetString(PyExc_KeyError, "key is not set.");
		return NULL;
	}
	return zv;
}

/**
Sets the value of the specified global variable.

*key* (``const char *``) is the name of the variable to set.

*keylen* (``int``) is the length of *key*.

*var* (``cosnt char *``) optionally sets the keyed value in the specified
global variable instead of in the global symbol table.

*varlen* (``int``) is the length of *var*.

*zv* (``zval *``) is the value of the variable to set.

.. NOTE: This reference is stolen.

Returns ``true`` on success; otherwise, ``false``.
*/
static bool pyphp_php_global_set(const char * key, int keylen, const char * var, int varlen, zval * zv) {
	HashTable * ht = NULL; // borrowed
	
	// Make sure PyPHP has been started.
	if (!pyphp.is_started) {
		PyErr_SetString(InternalErrorType, "PyPHP not initialized.");
		return false;
	}
	
	{
		TSRMLS_FETCH();
		if (var != NULL) {
			zval * zdict = NULL; // borrowed
			// Get hash table for specified var.
			// .. NOTE: Hash key length must include NULL byte.
			if (zend_symtable_find(&EG(symbol_table), var, (unsigned int)varlen + 1, &zdict) != SUCCESS) {
				PyErr_SetString(PyExc_KeyError, "var is not set.");
				return false;
			}
			ht = Z_ARRVAL_P(zdict);
		} else {
			// Get global symbol table.
			ht = &EG(symbol_table);
		}
	}
	
	// Set php value.
	// .. NOTE: Hash key length MUST include NULL byte.
	// .. NOTE: The zv reference is stolen.
	if (zend_symtable_update(ht, key, (unsigned int)keylen + 1, &zv, sizeof(zv), NULL) != SUCCESS) {
		PyErr_SetString(InternalErrorType, "Failed to set key/value.");
		return false;
	}
	return true;
}

/**
Gets the value of the specified configuration option.

*key* (``const char *``) is the name of the option to get.

*keylen* (``int``) is the length of *key*.

*orig* (``bool``) is whether the original configuration option should be
returned (``true``), or the current value (``false``).

Returns a borrowed reference to the option value (``const char *``).
*/
static const char * pyphp_php_ini_get(const char * key, int keylen, bool orig) {
	/*
	.. NOTE: This function is derived from ``ini_get()`` from
	   ``php-5.3.13/ext/standard/basic_functions.c``.
	*/
	char * val = NULL; // borrowed
	
	// Make sure PyPHP has been started.
	if (!pyphp.is_started) {
		PyErr_SetString(InternalErrorType, "PyPHP not initialized.");
		return NULL;
	}
	
	// Get INI value.
	// .. NOTE: I think it is safe to cast to `char *`.
	// .. NOTE: INI key length must include NULL byte.
	val = zend_ini_string((char *)key, (unsigned int)keylen + 1, (int)orig);
	if (val == NULL) {
		PyErr_SetString(PyExc_KeyError, "key is not set.");
		return NULL;
	}
	return val;
}

/*
NOTE: This function is copied from ``php_ini_get_option()`` from
``php-5.3.13/ext/standard/basic_functions.c``.
*/
static int php_ini_get_option(zend_ini_entry * ini_entry TSRMLS_DC, int num_args, va_list args, zend_hash_key * hash_key) {
	zval *ini_array = va_arg(args, zval *);
	int module_number = va_arg(args, int);
	int details = va_arg(args, int);
	zval *option;

	if (module_number != 0 && ini_entry->module_number != module_number) {
		return 0;
	}

	if (hash_key->nKeyLength == 0 ||
		hash_key->arKey[0] != 0
	) {
		if (details) {
			MAKE_STD_ZVAL(option);
			array_init(option);

			if (ini_entry->orig_value) {
				add_assoc_stringl(option, "global_value", ini_entry->orig_value, ini_entry->orig_value_length, 1);
			} else if (ini_entry->value) {
				add_assoc_stringl(option, "global_value", ini_entry->value, ini_entry->value_length, 1);
			} else {
				add_assoc_null(option, "global_value");
			}

			if (ini_entry->value) {
				add_assoc_stringl(option, "local_value", ini_entry->value, ini_entry->value_length, 1);
			} else {
				add_assoc_null(option, "local_value");
			}

			add_assoc_long(option, "access", ini_entry->modifiable);

			add_assoc_zval_ex(ini_array, ini_entry->name, ini_entry->name_length, option);
		} else {
			if (ini_entry->value) {
				add_assoc_stringl(ini_array, ini_entry->name, ini_entry->value, ini_entry->value_length, 1);
			} else {
				add_assoc_null(ini_array, ini_entry->name);
			}
		}
	}
	return 0;
}

/**
Gets all configuration options.

*ext* (``const char *``) optionally is the name of the specific extension to
get the configuration options for. If ``NULL`, all options will be returned.

*extlen* (``int``) is the length of *ext*.

*details* (``bool``) is whether detailed settings should be returned
(``true``), or not (``false``).

Returns a dict (``zval *``) mapping option key (``char *``) to value
(``char *``).
*/
static zval * pyphp_php_ini_get_all(const char * ext, int extlen, bool details) {
	/*
	.. NOTE: This function is derived from ``ini_get_all()`` and
	   ``php_ini_get_option()`` from
	   ``php-5.3.13/ext/standard/basic_functions.c``.
	*/
	zval * zdict = NULL; // owned
	
	// Make sure PyPHP has been started.
	if (!pyphp.is_started) {
		PyErr_SetString(InternalErrorType, "PyPHP not initialized.");
		return NULL;
	}
	
	{
		int zmodnum = 0;
		TSRMLS_FETCH();
		
		// Get php module extension ID.
		zend_ini_sort_entries(TSRMLS_C);
		if (ext != NULL) {
			zend_module_entry * zmod = NULL;
			if (zend_symtable_find(&module_registry, ext, extlen + 1, &zmod) != SUCCESS) {
				PyErr_SetString(PyExc_KeyError, "extension is not set.");
				return NULL;
			}
			zmodnum = zmod->module_number;
		}
		
		// Copy ini options to php dict.
		MAKE_STD_ZVAL(zdict);
		zend_hash_apply_with_arguments(EG(ini_directives) TSRMLS_CC, (apply_func_args_t)php_ini_get_option, 3, zdict, zmodnum, (int)details);
	}
	return zdict;
}

/**
Sets the value of the specified configuration option.

*key* (``const char *``) is the name of the option to set.

*keylen* (``int``) is the length of *key*.

*val* (``const char *``) is the value of the option to set.

*vallen (``int``) is the length of *val*.

Returns ``true`` on success; otherwise, ``false``.
*/
static bool pyphp_php_ini_set(const char * key, int keylen, const char * val, int vallen) {
	/*
	.. NOTE: This function is derived from ``ini_set()`` from
	   ``php-5.3.13/ext/standard/basic_functions.c``.
	*/
	
	// Make sure PyPHP has been started.
	if (!pyphp.is_started) {
		PyErr_SetString(InternalErrorType, "PyPHP not initialized.");
		return false;
	}
	
	// Set INI value.
	// .. NOTE: INI key length must include NULL byte but value length MUST NOT.
	if (zend_alter_ini_entry((char *)key, (unsigned int)keylen + 1, (char *)val, (unsigned int)vallen, PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME) != SUCCESS) {
		PyErr_SetString(InternalErrorType, "Failed to set option key/value.");
		return false;
	}
	return true;
}

/**
Sets the PHP output callback function.

*pyout* (``PyObject *``) is the output callback.

Returns ``true`` on success; otherwise, ``false``. 
*/
static bool pyphp_php_output_callback_set(PyObject * pyout) {
	// Make sure PyPHP has been started.
	if (!pyphp.is_started) {
		PyErr_SetString(InternalErrorType, "PyPHP not initialized.");
		return false;
	}
	
	// Set callback.
	Py_INCREF(pyout);
	Py_XDECREF(pyphp.pyout_cb);
	pyphp.pyout_cb = pyout;
	
	return true;
}

/**
Sets the PHP output file pointer.

*out_fp* (``int``) is the output file pointer.

Returns ``true`` on success; otherwise, ``false``.
*/
static bool pyphp_php_output_fp_set(FILE * out_fp) {
	// Make sure PyPHP has been started.
	if (!pyphp.is_started) {
		PyErr_SetString(InternalErrorType, "PyPHP not initialized.");
		return false;
	}
	
	// Set file pointer.
	pyphp.out_fp = out_fp;
	
	return true;
}

/**
Sets the PHP error callback function.

*pyerr* (``PyObject *``) is the error callback.

Returns ``true`` on success; otherwise, ``false``. 
*/
static bool pyphp_php_error_callback_set(PyObject * pyerr) {
	// Make sure PyPHP has been started.
	if (!pyphp.is_started) {
		PyErr_SetString(InternalErrorType, "PyPHP not initialized.");
		return false;
	}
	
	// Set callback.
	Py_INCREF(pyerr);
	Py_XDECREF(pyphp.pyerr_cb);
	pyphp.pyerr_cb = pyerr;
	
	return true;
}

/**
Sets the PHP error file pointer.

*err_fp* (``int``) is the error file pointer.

Returns ``true`` on success; otherwise, ``false``.
*/
static bool pyphp_php_error_fp_set(FILE * err_fp) {
	// Make sure PyPHP has been started.
	if (!pyphp.is_started) {
		PyErr_SetString(InternalErrorType, "PyPHP not initialized.");
		return false;
	}
	
	// Set file pointer.
	pyphp.err_fp = err_fp;
	
	return true;
}

/**
Sets the PHP log callback function.

*pylog* (``PyObject *``) is the log callback.

Returns ``true`` on success; otherwise, ``false``. 
*/
static bool pyphp_php_log_callback_set(PyObject * pylog) {
	// Make sure PyPHP has been started.
	if (!pyphp.is_started) {
		PyErr_SetString(InternalErrorType, "PyPHP not initialized.");
		return false;
	}
	
	// Set callback.
	Py_INCREF(pylog);
	Py_XDECREF(pyphp.pylog_cb);
	pyphp.pylog_cb = pylog;
	
	return true;
}

/**
Sets the PHP log file pointer.

*log_fp* (``int``) is the log file pointer.

Returns ``true`` on success; otherwise, ``false``.
*/
static bool pyphp_php_log_fp_set(FILE * log_fp) {
	// Make sure PyPHP has been started.
	if (!pyphp.is_started) {
		PyErr_SetString(InternalErrorType, "PyPHP not initialized.");
		return false;
	}
	
	// Set file pointer.
	pyphp.log_fp = log_fp;
	
	return true;
}


/**
Shuts-down PHP. PHP can be re-started after being shutdown.
*/
static void pyphp_php_shutdown() {
	if (!pyphp.is_started) {
		// Since PHP is not started, do nothing.
		return;
	}
	pyphp.is_started = false;
	
	php_request_shutdown(NULL);
}

/**
Called when PHP encounters an error.

.. TODO: Test to see if Python catches a fatal error raised by PHP.

*type* (``int``) is the error type.

*file* (``const char *``) is the file the error occured in.

*line* (``const unsigned int``) is the line the error occured on.

*format* (``const char *``) is the error message format.

*args* (``va_list``) is the error message arguments.
*/
static void pyphp_php_error_cb(int type, const char * file, const unsigned int line, const char * format, va_list args) {
	char * error = NULL;
	bool is_fatal = false;
	
	switch (type) {
		case E_ERROR:
			is_fatal = true;
			error = "Fatal Error";
			break;
		
		case E_CORE_ERROR:
			is_fatal = true;
			error = "Fatal Core Error";
			break;
		
		case E_COMPILE_ERROR:
			is_fatal = true;
			error = "Fatal Compile Error";
			break;
			
		case E_USER_ERROR:
			is_fatal = true;
			error = "Fatal User Error";
			break;
			
		case E_RECOVERABLE_ERROR:
			error = "Recoverable Error";
			break;
			
		case E_WARNING:
			error = "Warning";
			break;
		
		case E_CORE_WARNING:
			error = "Core Warning";
			break;
			
		case E_COMPILE_WARNING:
			error = "Compile Warning";
			break;
			
		case E_USER_WARNING:
			error = "User Warning";
			break;
			
		case E_PARSE:
			error = "Parse Error";
			break;
			
		case E_NOTICE:
			error = "Notice";
			break;
		
		case E_USER_NOTICE:
			error = "User Notice";
			break;
			
		case E_STRICT:
			error = "Strict Standards";
			break;
			
		case E_DEPRECATED:
			error = "Deprecated";
			break;
			
		case E_USER_DEPRECATED:
			error = "User Deprecated";
			break;
			
		default:
			error = "Unknown error";
			break;
	}
	
	// If the error is fatal, raise a python exception.
	if (is_fatal) {
		char * message = NULL;
		int msglen;
		va_list vars;
		
		// Copy args so that the arguments are not consumed before being passed to
		// internal PHP error handler.
		va_copy(vars, args);
		msglen = vspprintf(&message, PG(log_errors_max_len), format, vars);
		va_end(vars);
		if (message == NULL) {
			PyErr_SetString(InternalErrorType, "Failed to format PHP fatal error message.");
		} else {
			// Raise fatal error.
			PyObject * pyval = Py_BuildValue("(ss#sI)", error, message, (Py_ssize_t)msglen, file, line);
			if (pyval != NULL) {
				PyErr_SetObject(PhpFatalErrorType, pyval);
				Py_DECREF(pyval);
			}
			// Clean up.
			efree(message);
		}
	}
	
	// Call the internal PHP error handler.
	pyphp.php_internal_error_cb(type, file, line, format, args);
}

/**
Starts-up PHP.

*argc* (``int``) is the number of arguments.

*argv* (``char **``) is the arguments.

Returns ``true`` on success; otherwise, ``false`` and raises a Python
Exception.
*/
static bool pyphp_php_startup(int argc, char ** argv) {
	char * all_errors = NULL;
	int all_errors_len;
	
	if (pyphp.is_started) {
		// Since PHP is already started, do nothing.
		return true;
	}
	
	if (!pyphp.is_inited) {
		// Initialize the PHP embed SAPI.
		
		pyphp.out_fp = stdout;
		pyphp.err_fp = stdout;
		pyphp.log_fp = stdout;
	
		// Override PHP embed log handler.
		php_embed_module.log_message = pyphp_php_log_cb;

		// Override PHP embed output handler.
		php_embed_module.ub_write = pyphp_php_output_cb;
	
		// Override PHP embed output flush handler.
		php_embed_module.flush = pyphp_php_output_flush_cb; 
	
		// Override PHP embed startup handler.
		php_embed_module.startup = pyphp_php_startup_cb;
	
		// Completely initialize/startup PHP.
		if (php_embed_init(argc, argv PTSRMLS_CC) != SUCCESS) {
			// Raise internal error.
			PyErr_SetString(InternalErrorType, "Failed to initialize PHP embed SAPI.");
			return false;
		}
		pyphp.is_inited = true;
		
	} else {
		// Re-initialize PHP.
		if (php_request_startup(TSRMLS_C) != SUCCESS) {
			// Raise internal error.
			PyErr_SetString(InternalErrorType, "Failed to startup PHP.");
			return false;
		}
	}
	
	// Setup INI settings.
	all_errors_len = spprintf(&all_errors, 0, "%u", E_ALL);
	if (all_errors == NULL) {
		PyErr_Format(InternalErrorType, "Failed to convert E_ALL:%u to string.", E_ALL);
		goto startup_error;
	}
	// .. NOTE: INI key length MUST include NULL byte but value length MUST NOT.
	zend_alter_ini_entry("error_reporting", sizeof("error_reporting"), all_errors, all_errors_len, PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME);
	zend_alter_ini_entry("log_errors", sizeof("log_errors"), "1", sizeof("1") - 1, PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME);
	zend_alter_ini_entry("display_errors", sizeof("display_errors"), "1", sizeof("1") - 1, PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME);
	zend_alter_ini_entry("display_startup_errors", sizeof("display_startup_errors"), "1", sizeof("1") - 1, PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME);
	
	// Clean up.
	efree(all_errors);
	
	// PHP is fully started.
	pyphp.is_started = true;
	return true;
	
	startup_error: {
		// Shutdown PHP.
		pyphp_php_shutdown();
		return false;
	}
}

/**
Called when PHP logs a message.

*message* (``char *``) is the log message.
*/
static void pyphp_php_log_cb(char * message) {
	if (message == NULL) {
		return;
	}
	if (pyphp.log_fp != NULL) {
		// Write log to file pointer.
		fputs(message, pyphp.log_fp);
		fputc('\n', pyphp.log_fp);
		fflush(pyphp.log_fp);
	}
	if (pyphp.pylog_cb != NULL) {
		// Send log to callback.
		PyObject * pyargs = Py_BuildValue("(s):pyphp.pyphp_php_log_cb", message);
		if (pyargs != NULL) {
			PyObject * pyresult = PyEval_CallObject(pyphp.pylog_cb, pyargs);
			Py_XDECREF(pyresult);
			Py_DECREF(pyargs);
		}
	}
}

/**
Called when PHP outputs data.

*str* (``const char *``) is the output data.

*str_length* (``unsigned int``) is the number of bytes.

Returns the number of bytes written (``int``).
*/
static int pyphp_php_output_cb(const char * str, unsigned int str_length TSRMLS_DC) {
	int len;
	
	if (str == NULL || str_length == 0) {
		return 0;
	}
	len = str_length > INT_MAX ? INT_MAX : (int)str_length;
	if (pyphp.out_fp != NULL) {
		// Write data to file pointer.
		len = (int)fwrite(str, sizeof(*str), (size_t)len, pyphp.out_fp);
	}
	if (pyphp.pyout_cb != NULL) {
		// Send data to callback.
		PyObject * pyargs = Py_BuildValue("(s#):pyphp.pyphp_php_output_cb", str, (Py_ssize_t)len);
		if (pyargs != NULL) {
			PyObject * pyresult = PyEval_CallObject(pyphp.pyout_cb, pyargs);
			Py_XDECREF(pyresult);
			Py_DECREF(pyargs);
		}
	}
	return len;
}

/**
Called when the PHP output file pointer needs to be flushed.
*/
static void pyphp_php_output_flush_cb(void * server_ctx) {
	fflush(pyphp.out_fp);
}

/**
Restarts PHP.

Returns whether PHP was successfully restarted (``true``), or not (``false``).
*/
static bool pyphp_php_restart() {
	pyphp_php_shutdown();
	return pyphp_php_startup(0, NULL);
}

/**
Sets the PHP error handler Python callback function.

*pyerr_cb* (``PyObject *``) is the python callback.
*/
static void pyphp_php_set_error_cb(PyObject * pyerr_cb) {
	Py_XINCREF(pyerr_cb);
	Py_XDECREF(pyphp.pyerr_cb);
	pyphp.pyerr_cb = pyerr_cb;
}

/**
Sets the PHP log handler Python callback function.

*pylog_cb* (``PyObject *``) is the python callback.
*/
static void pyphp_php_set_log_cb(PyObject * pylog_cb) {
	Py_XINCREF(pylog_cb);
	Py_XDECREF(pyphp.pylog_cb);
	pyphp.pylog_cb = pylog_cb;
}

/**
Sets the PHP output handler Python callback function.

*pyout_cb* (``PyObject *``) is the python callback.
*/
static void pyphp_php_set_output_cb(PyObject * pyout_cb) {
	Py_XINCREF(pyout_cb);
	Py_XDECREF(pyphp.pyout_cb);
	pyphp.pyout_cb = pyout_cb;
}

/**
Completely, irreversibly shuts-down PHP.
*/
static void pyphp_php_destroy() {
	if (!pyphp.is_inited) {
		return;
	}
	pyphp.is_inited = false;
	pyphp.is_started = false;
	{
		TSRMLS_FETCH();
		php_embed_shutdown(TSRMLS_C);
	}
}

/**
Called when PHP is started up.

.. HACK: Stores and overrides the internal PHP error handler.

*sapi* (``sapi_module_struct *``) is the SAPI module to start up.

Returns ``SUCCESS`` on success; otherwise, ``FAILURE``.
*/
static int pyphp_php_startup_cb(sapi_module_struct * sapi) {
	if (php_module_startup(&sapi_module, NULL, 0) == FAILURE) {
		return FAILURE;
	}
	//HACK: Grab the function pointer to the internal PHP (zend) error handler.
	pyphp.php_internal_error_cb = zend_error_cb;
	//HACK: Override the internal PHP error handler.
	zend_error_cb = pyphp_php_error_cb;
	
	return SUCCESS;
}



/****************************** Module Methods ******************************/

static const char pyphp_exec_file_doc[] = (
	"Executes the specified PHP script.\n"
	"\n"
	"*file* (**string**) is the name of the file to execute.\n"
	"\n"
	".. NOTE: If *file* is ``unicode``, it will be encoded using the result\n"
	"   from ``sys.getfilesystemencoding()``. If an encoding other than that\n"
	"   is required, encode *file* to a binary ``str`` prior to sending it to\n"
	"   this method.\n" 
);

static PyObject * pyphp_exec_file(PyObject * self, PyObject * args) {
	PyObject * pyfile = NULL; // borrowed
	PyObject * pyfile_tmp = NULL; // owned
	FILE * fp = NULL; // owned
	bool result = false;
	
	if (!PyArg_ParseTuple(args, "O:pyphp.exec_file", &pyfile)) {
		return NULL;
	}
	if (PyString_Check(pyfile)) {
		Py_BEGIN_ALLOW_THREADS
		fp = fopen(PyString_AS_STRING(pyfile), "rb");
		Py_END_ALLOW_THREADS
		
	} else if (PyUnicode_Check(pyfile)) {
		pyfile_tmp = PyUnicode_AsEncodedString(pyfile, Py_FileSystemDefaultEncoding, "strict");
		if (pyfile_tmp == NULL) {
			return NULL;
		}
		
		#ifdef MS_WINDOWS
		// Require windows to have PY_UNICODE defined as wchar_t.
		// - http://mail.python.org/pipermail/python-dev/2004-October/049277.html
		# ifdef HAVE_USABLE_WCHAR_T
		Py_BEGIN_ALLOW_THREADS
		fp = _wfopen(PyUnicode_AS_UNICODE(pyfile), L"rb");
		Py_END_ALLOW_THREADS
		# else
		#  error "Py_UNICODE must be wchar_t on Windows."
		# endif
		#else
		Py_BEGIN_ALLOW_THREADS
		fp = fopen(PyString_AS_STRING(pyfile_tmp), "rb");
		Py_END_ALLOW_THREADS
		#endif
		
	} else {
		PyErr_Format(PyExc_TypeError, "file:%s is not a string.", Py_TYPE(pyfile)->tp_name);
		return NULL;
	}
	if (fp == NULL) {
		PyErr_SetFromErrnoWithFilenameObject(PyExc_IOError, pyfile);
		return NULL;
	}
	
	// Execute file.
	// .. NOTE: The file pointer is stolen.
	{
		PyObject * pystr = pyfile_tmp != NULL ? pyfile_tmp : pyfile; // borrowed
		result = pyphp_php_exec_file(PyString_AS_STRING(pystr), PyString_GET_SIZE(pystr), fp);
	}
	Py_XDECREF(pyfile_tmp);
	if (!result) {
		return NULL;
	}
	
	Py_RETURN_NONE;
}

static const char pyphp_exec_inline_doc[] = (
	"Executes the specified PHP inline string/script.\n"
	"\n"
	"*string* (``str``) is the string to execute.\n"
	"\n"
	"*name* (``str``) optionally is the name to use in the case of an error.\n"
);

static PyObject * pyphp_exec_inline(PyObject * self, PyObject * args) {
	const char * str = NULL;
	const char * name = NULL;
	Py_ssize_t str_len = 0;
	
	if (!PyArg_ParseTuple(args, "s#|z:pyphp.exec_inline", &str, &str_len, &name)) {
		return NULL;
	}
	if (str_len < 0 || INT_MAX < str_len) {
		PyErr_Format(PyExc_ValueError, "string length:%" PY_Z "i must be between 0 and %i inclusive.", str_len, INT_MAX);
		return NULL;
	}
	
	// Execute string.
	if (!pyphp_php_exec_inline(name, str, str_len)) {
		return NULL;
	}
	
	Py_RETURN_NONE;
}

static const char pyphp_global_get_doc[] = (
	"Gets the value of the specified global variable.\n"
	"\n"
	"*key* (``str``) is the name of the variable to get.\n"
	"\n"
	"*var* (``str``) optionally gets the keyed value from the specified\n"
	"global variable instead of from the global symbol table. Default is\n"
	"``None``."
	"\n"
	"Returns the value (**mixed**) of the global variable."
);

static PyObject * pyphp_global_get(PyObject * self, PyObject * args) {
	const char * key = NULL; // borrowed
	const char * var = NULL; // borrowed
	Py_ssize_t keylen = 0;
	Py_ssize_t varlen = 0;
	zval * zv = NULL; // borrowed
	
	if (!PyArg_ParseTuple(args, "s#|z#:pyphp.global_get", &key, &keylen, &var, &varlen)) {
		return NULL;
	}
	if (keylen < 0 || INT_MAX < keylen) {
		PyErr_Format(PyExc_ValueError, "key length:%" PY_Z "i must be between 0 and %i inclusive.", keylen, INT_MAX);
		return NULL;
	}
	if (varlen < 0 || INT_MAX < varlen) {
		PyErr_Format(PyExc_ValueError, "var length:%" PY_Z "i must be between 0 and %i inclusive.", varlen, INT_MAX);
		return NULL;
	}
	
	// Get global.
	zv = pyphp_php_global_get(key, (int)keylen, var, (int)varlen);
	if (zv == NULL) {
		return NULL;
	}
	
	// Convert php value to python value.
	return zval_to_PyObject(zv, NULL);
}

static const char pyphp_global_set_doc[] = (
	"Sets the value of the specified global variable.\n"
	"\n"
	"*key* (``str``) is the name of the variable to set.\n"
	"\n"
	"*value* (``str``) is the value of the variable to set.\n"
	"\n"
	"*var* (``str``) optionally sets the keyed value in the specified global\n"
	"variable instead of in the global symbol table. Default is ``None``.\n"
);

static PyObject * pyphp_global_set(PyObject * self, PyObject * args) {
	const char * key = NULL; // borrowed
	const char * var = NULL; // borrowed
	Py_ssize_t keylen = 0;
	Py_ssize_t varlen = 0;
	PyObject * pyval = NULL; // borrowed
	zval * zv = NULL; // owned
	
	if (!PyArg_ParseTuple(args, "s#O|z#:pyphp.global_set", &key, &keylen, &pyval, &var, &varlen)) {
		return NULL;
	}
	if (keylen < 0 || INT_MAX < keylen) {
		PyErr_Format(PyExc_ValueError, "key length:%" PY_Z "i must be between 0 and %i inclusive.", keylen, INT_MAX);
		return NULL;
	}
	if (varlen < 0 || INT_MAX < varlen) {
		PyErr_Format(PyExc_ValueError, "var length:%" PY_Z "i must be between 0 and %i inclusive.", varlen, INT_MAX);
		return NULL;
	}
	
	// Convert python value to php value.
	zv = PyObject_to_zval(pyval, NULL);
	if (zv == NULL) {
		return NULL;
	}
	
	// Set global.
	// .. NOTE: zv reference is stolen.
	if (!pyphp_php_global_set(key, (int)keylen, var, (int)varlen, zv)) {
		// Clean up.
		zval_del(&zv);
		return NULL;
	}
	
	Py_RETURN_NONE;
}

static const char pyphp_ini_get_doc[] = (
	"Gets the value of the specified configuration option.\n"
	"\n"
	"*key* (``str``) is the name of the option to get.\n"
	"\n"
	"*orig* (``bool``) is whether the original configuration option should be\n"
	"returned (``True``), or the current value (``False``). Default is\n"
	"``False``.\n"
	"\n"
	"Returns the value (``str``) of the option configuration." 
);

static PyObject * pyphp_ini_get(PyObject * self, PyObject * args) {
	const char * key = NULL; // borrowed
	Py_ssize_t keylen = 0;
	int orig = 0;
	const char * val = NULL; // borrowed
	
	if (!PyArg_ParseTuple(args, "s#|i:pyphp.ini_get", &key, &keylen, &orig)) {
		return NULL;
	}
	if (keylen < 0 || INT_MAX < keylen) {
		PyErr_Format(PyExc_ValueError, "key length:%" PY_Z "i must be between 0 and %i inclusive.", keylen, INT_MAX);
		return NULL;
	}
	
	// Get ini value.
	val = pyphp_php_ini_get(key, (int)keylen, (bool)orig);
	if (val == NULL) {
		return NULL;
	}
	return PyString_FromString(val);
}

static const char pyphp_ini_get_all_doc[] = (
	"Gets all configuration options.\n"
	"\n"
	"*extension* (``str``) is the name of the specific extension for which\n"
	"its options will be returned. Default is ``None`` to return all options.\n"
	"\n"
	"*details* (``bool``) is whether detailed settings should be returned\n"
	"(``True``), or not (``False``). Default is ``False``.\n"
	"\n"
	"Returns a ``dict`` mapping option key (``str``) to value (``str``)."
);

static PyObject * pyphp_ini_get_all(PyObject * self, PyObject * args) {
	const char * ext = NULL; // borrowed
	Py_ssize_t extlen = 0;
	int details = 0;
	zval * zdict = NULL; // owned
	PyObject * pydict = NULL; // owned
	
	if (!PyArg_ParseTuple(args, "|z#i:pyphp.ini_get_all", &ext, &extlen, &details)) {
		return NULL;
	}
	if (extlen < 0 || INT_MAX < extlen) {
		PyErr_Format(PyExc_ValueError, "extension length:%" PY_Z "i must be between 0 and %i inclusive.", extlen, INT_MAX);
		return NULL;
	}
	
	// Make sure PyPHP has been started.
	if (!pyphp.is_started) {
		PyErr_SetString(InternalErrorType, "PyPHP not initialized.");
		return NULL;
	}
	
	// Get options.
	zdict = pyphp_php_ini_get_all(ext, (int)extlen, (bool)details);
	if (zdict == NULL) {
		return NULL;
	}
	
	// Convert php dict to python dict.
	pydict = zval_to_PyObject(zdict, NULL);
	zval_del(&zdict);
	return pydict;
}

static const char pyphp_ini_set_doc[] = (
	"Sets the value of the specified configuration option.\n"
	"\n"
	"*key* (``str``) is the name of the option to set.\n"
	"\n"
	"*value* (``str``) is the value of the option to set." 
);

static PyObject * pyphp_ini_set(PyObject * self, PyObject * args) {
	const char * key = NULL; // borrowed
	const char * val = NULL; // borrowed
	Py_ssize_t keylen = 0;
	Py_ssize_t vallen = 0;
	
	if (!PyArg_ParseTuple(args, "s#s#:pyphp.ini_set", &key, &keylen, &val, &vallen)) {
		return NULL;
	}
	if (keylen < 0 || INT_MAX < keylen) {
		PyErr_Format(PyExc_ValueError, "key length:%" PY_Z "i must be between 0 and %i inclusive.", keylen, INT_MAX);
		return NULL;
	}
	if (vallen < 0 || INT_MAX < vallen) {
		PyErr_Format(PyExc_ValueError, "value length:%" PY_Z "i must be between 0 and %i inclusive.", vallen, INT_MAX);
		return NULL;
	}
	
	// Set INI value.
	if (!pyphp_php_ini_set(key, (int)keylen, val, (int)vallen)) {
		return NULL;
	}
	
	Py_RETURN_NONE;
}

static const char pyphp_init_doc[] = (
	"Starts-up the PHP interpreter."
);

static PyObject * pyphp_init(PyObject * self, PyObject * args) {
	if (!pyphp_php_startup(0, NULL)) {
		return NULL;
	}
	Py_RETURN_NONE;
}

static const char pyphp_reset_doc[] = (
	"Resets the PHP interpreter."
);

static PyObject * pyphp_reset(PyObject * self, PyObject * args) {
	if (!pyphp_php_restart(0, NULL)) {
		return NULL;
	}
	Py_RETURN_NONE;
}

static const char pyphp_shutdown_doc[] = (
	"Shuts-down the PHP interpreter."
);

static PyObject * pyphp_shutdown(PyObject * self, PyObject * args) {
	pyphp_php_shutdown();

	Py_RETURN_NONE;
}

static const char pyphp_output_callback_set_doc[] = (
	"Sets the PHP output callback function.\n"
	"\n"
	"*callback* (**callable**) is the output callback.\n"
);

static PyObject * pyphp_output_callback_set(PyObject * self, PyObject * args) {
	PyObject * pyout = NULL;
	
	if (!PyArg_ParseTuple(args, "O:pyphp.set_output_callback", &pyout)) {
		return NULL;
	}
	if (!PyCallable_Check(pyout)) {
		PyErr_Format(PyExc_TypeError, "callback:%s is not callable.", Py_TYPE(pyout)->tp_name);
		return NULL;
	}
	
	// Set callback.
	if (!pyphp_php_output_callback_set(pyout)) {
		return NULL;
	}
	
	Py_RETURN_NONE;
}

static const char pyphp_output_fd_set_doc[] = (
	"Sets the PHP output file descriptor.\n"
	"\n"
	"*fd* (``int``) is the output file descriptor. Set to -1 for no file\n"
	"descriptor."
);

static PyObject * pyphp_output_fd_set(PyObject * self, PyObject * args) {
	int out_fd = -1;
	FILE * out_fp = NULL;
	
	if (!PyArg_ParseTuple(args, "i:pyphp.set_output_fd", &out_fd)) {
		return NULL;
	}
	
	// Open file pointer.
	if (out_fd != -1) {
		out_fp = fdopen(out_fd, "rb");
		if (out_fp == NULL) {
			PyErr_SetFromErrno(PyExc_IOError);
			return NULL;
		}
		
		// Set file pointer.
		if (!pyphp_php_output_fp_set(out_fp)) {
			return NULL;
		}
	}
	
	Py_RETURN_NONE;
}

static const char pyphp_error_callback_set_doc[] = (
	"Sets the PHP error callback function.\n"
	"\n"
	"*callback* (**callable**) is the error callback."
);

static PyObject * pyphp_error_callback_set(PyObject * self, PyObject * args) {
	PyObject * pyerr = NULL;
	
	if (!PyArg_ParseTuple(args, "O:pyphp.set_error_callback", &pyerr)) {
		return NULL;
	}
	if (!PyCallable_Check(pyerr)) {
		PyErr_Format(PyExc_TypeError, "callback:%s is not callable.", Py_TYPE(pyerr)->tp_name);
		return NULL;
	}
	
	// Set callback.
	if (!pyphp_php_output_callback_set(pyerr)) {
		return NULL;
	}
	
	Py_RETURN_NONE;
}

static const char pyphp_error_fd_set_doc[] = (
	"Sets the PHP error file descriptor.\n"
	"\n"
	"*fd* (``int``) is the error file descriptor. Set to -1 for no file\n"
	"descriptor."
);

static PyObject * pyphp_error_fd_set(PyObject * self, PyObject * args) {
	int err_fd = -1;
	FILE * err_fp = NULL;
	
	if (!PyArg_ParseTuple(args, "i:pyphp.set_error_fd", &err_fd)) {
		return NULL;
	}
	
	// Open file pointer.
	if (err_fd != -1) {
		err_fp = fdopen(err_fd, "rb");
		if (err_fp == NULL) {
			PyErr_SetFromErrno(PyExc_IOError);
			return NULL;
		}
		
		// Set file pointer.
		if (!pyphp_php_output_fp_set(err_fp)) {
			return NULL;
		}
	}
	
	Py_RETURN_NONE;
}

static const char pyphp_log_callback_set_doc[] = (
	"Sets the PHP log callback function.\n"
	"\n"
	"*callback* (**callable**) is the log callback."
);

static PyObject * pyphp_log_callback_set(PyObject * self, PyObject * args) {
	PyObject * pylog = NULL;
	
	if (!PyArg_ParseTuple(args, "O:pyphp.set_log_callback", &pylog)) {
		return NULL;
	}
	if (!PyCallable_Check(pylog)) {
		PyErr_Format(PyExc_TypeError, "callback:%s is not callable.", Py_TYPE(pylog)->tp_name);
		return NULL;
	}
	
	// Set callback.
	if (!pyphp_php_output_callback_set(pylog)) {
		return NULL;
	}
	
	Py_RETURN_NONE;
}

static const char pyphp_log_fd_set_doc[] = (
	"Sets the PHP log file descriptor.\n"
	"\n"
	"*fd* (``int``) is the log file descriptor. Set to -1 for no file\n"
	"descriptor."
);

static PyObject * pyphp_log_fd_set(PyObject * self, PyObject * args) {
	int log_fd = -1;
	FILE * log_fp = NULL;
	
	if (!PyArg_ParseTuple(args, "i:pyphp_set_output_fd", &log_fd)) {
		return NULL;
	}
	
	// Open file pointer.
	if (log_fd != -1) {
		log_fp = fdopen(log_fd, "rb");
		if (log_fp == NULL) {
			PyErr_SetFromErrno(PyExc_IOError);
			return NULL;
		}
		
		// Set file pointer.
		if (!pyphp_php_output_fp_set(log_fp)) {
			return NULL;
		}
	}
	
	Py_RETURN_NONE;
}



/********************************** Module **********************************/

static const char module_doc[] = (
	"This module provides an interface to the embedded PHP interpreter."
);

static PyMethodDef module_methods[] = {
	{"exec_file", pyphp_exec_file, METH_VARARGS, pyphp_exec_file_doc},
	{"exec_inline", pyphp_exec_inline, METH_VARARGS, pyphp_exec_inline_doc},
	{"global_get", pyphp_global_get, METH_VARARGS, pyphp_global_get_doc},
	{"global_set", pyphp_global_set, METH_VARARGS, pyphp_global_set_doc},
	{"ini_get", pyphp_ini_get, METH_VARARGS, pyphp_ini_get_doc},
	{"ini_get_all", pyphp_ini_get_all, METH_VARARGS, pyphp_ini_get_all_doc},
	{"ini_set", pyphp_ini_set, METH_VARARGS, pyphp_ini_set_doc},
	{"init", pyphp_init, METH_NOARGS, pyphp_init_doc},
	{"reset", pyphp_reset, METH_NOARGS, pyphp_reset_doc},
	{"shutdown", pyphp_shutdown, METH_NOARGS, pyphp_shutdown_doc},
	{"set_output_callback", pyphp_output_callback_set, METH_VARARGS, pyphp_output_callback_set_doc},
	{"set_output_fd", pyphp_output_fd_set, METH_VARARGS, pyphp_output_fd_set_doc},
	{"set_error_callback", pyphp_error_callback_set, METH_VARARGS, pyphp_error_callback_set_doc},
	{"set_error_fd", pyphp_error_fd_set, METH_VARARGS, pyphp_error_fd_set_doc},
	{"set_log_callback", pyphp_log_callback_set, METH_VARARGS, pyphp_log_callback_set_doc},
	{"set_log_fd", pyphp_log_fd_set, METH_VARARGS, pyphp_log_fd_set_doc},
	{NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC initcpyphp() {
	PyObject * module;
	
	// Initialize module.
	module = Py_InitModule3("cpyphp", module_methods, module_doc);
	if (module == NULL) {
		return;
	}
	
	// PyPHP Exception type.
	PyphpExceptionType = PyErr_NewExceptionWithDoc("cpyphp.PyphpException", (char *)PyphpExceptionType_doc, NULL, NULL);
	if (PyphpExceptionType == NULL) {
		return;
	}
	if (PyModule_AddObject(module, "PyphpException", PyphpExceptionType) != 0) {
		return;
	}
	
	// Internal Error type.
	InternalErrorType = PyErr_NewExceptionWithDoc("cpyphp.InternalError", (char *)InternalErrorType_doc, PyphpExceptionType, NULL);
	if (InternalErrorType == NULL) {
		return;
	}
	if (PyModule_AddObject(module, "InternalError", InternalErrorType) != 0) {
		return;
	}
	
	// PHP Fatal Error type.
	PhpFatalErrorType = PyErr_NewExceptionWithDoc("cpyphp.PhpFatalError", (char *)PhpFatalErrorType_doc, PyphpExceptionType, NULL);
	if (PhpFatalErrorType == NULL) {
		return;
	}
	if (PyModule_AddObject(module, "PhpFatalError", PhpFatalErrorType) != 0) {
		return;
	}
}
