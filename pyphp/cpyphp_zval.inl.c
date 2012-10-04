/**
This module contains functions for handling PHP (zend) values. All of the
functions defined within this module are meant to be local (static) to the
including module so that the exported namespace is not poluted.

:Authors: Caleb P. Burns <cpburnz@gmail.com>; Ben DeMott <ben_demott@hotmail.com>
:Version: 0.5
:Status: Development
:Date: 2012-06-21
*/

#include <limits.h> // INT_MAX
#include <stdbool.h> // bool, false, true
#include <stdlib.h> // strtol
#include <string.h> // strlen

#include <main/spprintf.h> // spprintf
#include <Zend/zend.h> // zval, ALLOC_ZVAL, MAKE_STD_ZVAL
#include <Zend/zend_alloc.h> // estrndup
#include <Zend/zend_hash.h> // zend_hash_num_elements
#include <Zend/zend_operators.h> // convert_to_*, zend_dval_to_lval, zend_stdtod, Z_*_P
#include <Zend/zend_variables.h> // zval_copy_ctor, zval_ptr_dtor

/**
Returns a copy of the specified PHP value.

*zv* (``zval *``) is the PHP value to copy.

.. NOTE: *zv* is simply copied so you are still responsible for properly
   destroying it.

Returns the copied PHP value (``zval *``).
*/
static zval * zval_copy(zval * zv) {
	/*
	.. NOTE: This function is derived from ``ZVAL_ZVAL()`` in
	   ``php-5.3.13/Zend/zend_API.h``.
	*/
	zval * copy = NULL;
	
	if (zv == NULL) {
		return NULL;
	}
	
	// Initialize and copy zval.
	ALLOC_ZVAL(copy);
	if (copy != NULL) {
		*copy = *zv;
		zval_copy_ctor(copy);
	}
	return copy;
}

/**
Deletes the specified PHP value.

*zp* (``zval **``) is a pointer to the PHP value to delete.
*/
static void zval_del(zval ** zp) {
	zval_ptr_dtor(zp);
}

/**
Returns a new PHP value for the specified boolean.

*value* (``bool``) is the boolean to use.

Returns the new PHP bool (``zval *``).
*/
static zval * zval_from_bool(bool value) {
	/*
	.. NOTE: This function is derived from ``ZVAL_BOOL()`` in
	   ``php-5.3.13/Zend/zend_API.h``.
	*/
	zval * zv = NULL;
	
	// Initialize zval.
	MAKE_STD_ZVAL(zv);
	if (zv != NULL) {
		Z_TYPE_P(zv) = IS_BOOL;
		Z_LVAL_P(zv) = value ? 1 : 0;
	}
	return zv;
}

/**
Returns a new PHP value for the specified double floating-point.

*value* (``double``) is the double floating-point.

Returns the new PHP double (``zval *``).
*/
static zval * zval_from_double(double value) {
	/*
	.. NOTE: This function is derived from ``ZVAL_DOUBLE()`` in
	  ``php-5.3.13/Zend/zend_API.h``.
	*/
	zval * zv = NULL;
	
	// Initialize zval.
	MAKE_STD_ZVAL(zv);
	if (zv != NULL) {
		Z_TYPE_P(zv) = IS_DOUBLE;
		Z_DVAL_P(zv) = value;
	}
	return zv;
}

/**
Returns a new PHP variable for the specified long integer.

*value* (``long``) is the long integer.

Returns the new PHP long (``zval *``).
*/
static zval * zval_from_long(long value) {
	/*
	.. NOTE: This function is derived from ``ZVAL_LONG()`` in
	   ``php-5.3.13/Zend/zend_API.h``.
	*/
	zval * zv = NULL;
	
	// Initialize zval.
	MAKE_STD_ZVAL(zv);
	if (zv != NULL) {
		Z_TYPE_P(zv) = IS_LONG;
		Z_LVAL_P(zv) = value;
	}
	return zv;
}

/**
Returns a new PHP variable for null.

Returns the new PHP null (``zval *``).
*/
static zval * zval_from_null() {
	/*
	.. NOTE: This function is derived from ``ZVAL_NULL()`` in
	   ``php-5.3.13/Zend/zend_API.h``.
	*/
	zval * zv = NULL;
	
	// Initialize zval.
	MAKE_STD_ZVAL(zv);
	if (zv != NULL) {
		Z_TYPE_P(zv) = IS_NULL;
		Z_LVAL_P(zv) = 0;
	}
	return zv;
}

/**
Returns a new PHP variable for the specified string.

*value* (``const char *``) is the string.

.. NOTE: *value* is simply copied so you are still responsible for freeing its
   memory.
   
*length* (``int``) is the length of the string. If this is less than 0, the
string length will be calculated.

Returns the new PHP string (``zval *``).
*/
static zval * zval_from_string(const char * value, int length) {
	/*
	.. NOTE: This function is derived from ``ZVAL_STRING()`` in
	   ``php-5.3.13/Zend/zend_API.h``.
	*/
	zval * zv = NULL; // owned
	char * tmp; // owned
	
	if (value == NULL) {
		return NULL;
	}
	if (length < 0) {
		size_t len = strlen(value);
		if (len > INT_MAX) {
			return NULL;
		}
		length = (int)len;
	}
	
	// Initialize zval.
	MAKE_STD_ZVAL(zv);
	if (zv != NULL) {
		tmp = estrndup(value, (unsigned int)length);
		if (tmp == NULL) {
			zval_del(&zv);
			return NULL;
		}
		Z_TYPE_P(zv) = IS_STRING;
		Z_STRVAL_P(zv) = tmp;
		Z_STRLEN_P(zv) = length;
	}
	return zv;
}

/**
Determines whether the PHP array is a list.

*zarray* (``zval *``) is the PHP array.

Returns ``true`` if the PHP array is a list; otherwise, ``false``.
*/
static bool zval_is_list(zval * zarray) {
	/*
	.. NOTE: This function is derived from ``zend_hash_apply_with_arguments``
	   from ``php-5.3.13/Zend/zend_hash.c``.
	*/
	HashTable * ht = NULL; // borrowed
	Bucket * p = NULL; // borrowed
	unsigned long len = 0;
	
	ht = Z_ARRVAL_P(zarray);
	len = (unsigned long)zend_hash_num_elements(Z_ARRVAL_P(zarray));
	p = ht->pListHead;
	while (p != NULL) {
		// Numeric indices are maked by nKeyLength == 0 and stored in h.
		if (p->nKeyLength != 0 || p->h >= len) {
			return false;
		}
		p = p->pListNext;
	}
	return true;
}

/**
Converts the specified PHP value into a boolean.

*zv* (``zval *``) is the PHP value.

*dest* (``bool *``) is where to store the converted value.

Returns ``true`` on success; otherwise, ``false``.
*/
static bool zval_to_bool(zval * zv, bool * dest) {
	/*
	.. NOTE: This function is derived from ``convert_to_boolean()`` in
	   ``php-5.3.13/Zend/zend_operators.c``.
	*/
	if (zv == NULL || dest == NULL) {
		return false;
	}
	switch (Z_TYPE_P(zv)) {
		case IS_NULL:
			*dest = false;
			return true;
	
		case IS_BOOL:
		case IS_LONG:
		case IS_RESOURCE:
			*dest = Z_LVAL_P(zv) ? true : false;
			return true;
			
		case IS_DOUBLE:
			*dest = Z_DVAL_P(zv) ? true : false;
			return true;
			
		case IS_STRING:
			*dest = (Z_STRLEN_P(zv) == 0 || (Z_STRLEN_P(zv) == 1 && Z_STRVAL_P(zv)[0] == '0')) ? true : false;
			return true;
		
		case IS_ARRAY:
			*dest = zend_hash_num_elements(Z_ARRVAL_P(zv)) ? true : false;
			return true;
		
		case IS_OBJECT: {
			zval * copy = zval_copy(zv);
			if (copy != NULL) {
				convert_to_boolean(copy);
				if (Z_TYPE_P(copy) == IS_BOOL) {
					*dest = Z_LVAL_P(copy) ? true : false;
					zval_del(&copy);
					return true;
				} 
				zval_del(&copy);
			}
		} break;
		
		default:
			break;
	}
	return false;
}

/**
Converts the specified PHP value to a double float-point.

*zv* (``zval *``) is the PHP value.

*dest* (``double *``) is where to store the converted value.

Returns ``true`` on success; otherwise, ``false``.
*/
static bool zval_to_double(zval * zv, double * dest) {
	/*
	.. NOTE: This function is derived from ``convert_to_double()`` in
	   ``php-5.3.13/Zend/zend_operators.c``.
	*/
	if (zv == NULL || dest == NULL) {
		return false;
	}
	switch (Z_TYPE_P(zv)) {
		case IS_NULL:
			*dest = 0.0;
			return true;
	
		case IS_BOOL:
		case IS_LONG:
		case IS_RESOURCE:
			*dest = (double)Z_LVAL_P(zv);
			return true;
			
		case IS_DOUBLE:
			*dest = Z_DVAL_P(zv);
			return true;
			
		case IS_STRING:
			*dest = zend_strtod(Z_STRVAL_P(zv), NULL);
			return true;
			
		case IS_ARRAY:
			*dest = zend_hash_num_elements(Z_ARRVAL_P(zv)) ? 1.0 : 0.0;
			return true;
			
		case IS_OBJECT: {
			zval * copy = zval_copy(zv);
			if (copy != NULL) {
				convert_to_double(copy);
				if (Z_TYPE_P(copy) == IS_DOUBLE) {
					*dest = Z_DVAL_P(copy);
					zval_del(&copy);
					return true;
				}
				zval_del(&copy);
			}
		} break;
		
		default:
			break;
	}
	return false;
}

/**
Converts the specified PHP value to a long integer.

*zv* (``zval *``) is the PHP value.

*dest* (``long *``) is where to store the converted value.

Returns ``true`` on success; otherwise, ``false``.
*/
static bool zval_to_long(zval * zv, long * dest) {
	/*
	.. NOTE: This function is derived from ``convert_to_long_base()`` in
	   ``php-5.3.13/Zend/zend_operators.c``.
	*/
	if (zv == NULL || dest == NULL) {
		return false;
	}
	switch (Z_TYPE_P(zv)) {
		case IS_NULL:
			*dest = 0;
			return true;
			
		case IS_BOOL:
		case IS_LONG:
		case IS_RESOURCE:
			*dest = Z_LVAL_P(zv);
			return true;
			
		case IS_DOUBLE:
			*dest = zend_dval_to_lval(Z_DVAL_P(zv));
			return true;
			
		case IS_STRING:
			*dest = strtol(Z_STRVAL_P(zv), NULL, 10);
			return true;
			
		case IS_ARRAY:
			*dest = zend_hash_num_elements(Z_ARRVAL_P(zv)) ? 1 : 0;
			return true;
			
		case IS_OBJECT: {
			zval * copy = zval_copy(zv);
			if (copy != NULL) {
				convert_to_long(copy);
				if (Z_TYPE_P(copy) == IS_LONG) {
					*dest = Z_LVAL_P(copy);
					zval_del(&copy);
					return true;
				}
				zval_del(&copy);
			}
		} break;
		
		default:
			break;
	}
	return 0;
}

/**
Converts the specified PHP value to a string.

*zv* (``zval *``) is the PHP value.

*dest* (``char **``) is where the store the converted value.

.. NOTE: You are responsible for destroying the string (``char *``) with the
   PHP function ``efree()``.

Returns ``true`` on success; otherwise, ``false``.
*/
static bool zval_to_string(zval * zv, char ** dest) {
	/*
	.. NOTE: This function is derived from ``convert_to_long_base()`` in
	   ``php-5.3.13/Zend/zend_operators.c``.
	*/
	char * tmp = NULL;
	if (zv == NULL || dest == NULL) {
		return false;
	}
	switch (Z_TYPE_P(zv)) {
		case IS_NULL:
			tmp = estrndup("", sizeof("") - 1);
			if (tmp != NULL) {
				*dest = tmp;
				return true;
			}
			break;
		
		case IS_BOOL:
			if (Z_LVAL_P(zv)) {
				tmp = estrndup("1", sizeof("1") - 1);
			} else {
				tmp = estrndup("", sizeof("") - 1);
			}
			if (tmp != NULL) {
				*dest = tmp;
				return true;
			}
			break;
			
		case IS_LONG:
			spprintf(&tmp, 0, "%ld", Z_LVAL_P(zv));
			if (tmp != NULL) {
				*dest = tmp;
				return true;
			}
			break;
		
		case IS_DOUBLE: {
			TSRMLS_FETCH();
			spprintf(&tmp, 0, "%.*G", (int)EG(precision), Z_DVAL_P(zv));
			if (tmp != NULL) {
				*dest = tmp;
				return true;
			}
		} break;
			
		case IS_STRING:
			tmp = estrndup(Z_STRVAL_P(zv), Z_STRLEN_P(zv));
			if (tmp != NULL) {
				*dest = tmp;
				return true;
			}
			break;
			
		case IS_ARRAY:
			tmp = estrndup("Array", sizeof("Array") - 1);
			if (tmp != NULL) {
				*dest = tmp;
				return true;
			}
			break;
			
		case IS_OBJECT: {
			zval * copy = zval_copy(zv);
			if (copy != NULL) {
				convert_to_string(copy);
				if (Z_TYPE_P(copy) == IS_STRING) {
					tmp = estrndup(Z_STRVAL_P(copy), Z_STRLEN_P(copy));
					if (tmp != NULL) {
						*dest = tmp;
						zval_del(&copy);
						return true;
					}
				}
				zval_del(&copy);
			}
		} break;
			
		case IS_RESOURCE: {
			spprintf(&tmp, 0, "Resource id #%ld", Z_LVAL_P(zv));
			if (tmp != NULL) {
				*dest = tmp;
				return true;
			}
		} break;
		
		default:
			break;
	}
	return false;
}
