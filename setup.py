# coding: utf-8

__author__ = "Caleb Burns"
__version__ = "0.5.0"
__status__ = "Development"

import collections
import os
import os.path
import platform
import warnings
from distutils.core import setup, Extension
from distutils.command.install import INSTALL_SCHEMES

# Change data path to packages path.
for scheme in INSTALL_SCHEMES.itervalues():
	scheme['data'] = scheme['purelib']

system = platform.system()
arch = platform.architecture()[0]

# TODO: These need to be configurable.
php_include_path = os.environ['PHP_INCLUDE_PATH']
php_library_path = os.environ['PHP_LIBRARY_PATH']

defines = []
sources = [
	'pyphp/cpyphp_module.c'
]
libraries = []
include_dirs = [
	'inc',
	'inc/std',
	php_include_path,
	os.path.join(php_include_path, 'main'),
	os.path.join(php_include_path, 'Zend'),
	os.path.join(php_include_path, 'TSRM')
]
library_dirs = [
	php_library_path
]
runtime_library_dirs = []
extra_compile_args = []
data_files = collections.defaultdict(list)

if system == 'Windows':
	if arch != '32bit':
		warnings.warn("Only Windows 32bit has been tested: your architecture is %r." % arch)
	
	defines += [
		('ZEND_WIN32', None),
		('PHP_WIN32', None),
		('_USE_32BIT_TIME_T', None)
	]
	libraries += [
		'php5',
		'php5embed'
	]
	data_files['pyphp'] += [
		os.path.join(php_library_path, 'php5.dll')
	]
	
else:
	if system != 'Linux':
		warnings.warn("Your system %r has not been tested. Trying Linux configuration." % system)
		
	libraries += [
		'php5'
	]
	extra_compile_args += [
		'-std=c99'
	]
	data_files['pyphp'] += [
		os.path.join(php_library_path, 'libphp5.so')
	]

pyphp_ext = Extension(
	name='pyphp.cpyphp',
	sources=sources,
	include_dirs=include_dirs,
	define_macros=defines,
	library_dirs=library_dirs,
	libraries=libraries,
	runtime_library_dirs=runtime_library_dirs,
	extra_compile_args=extra_compile_args
)

setup(
	name='pyphp',
	version=__version__,
	author="Caleb P. Burns",
	author_email="cpburnz@gmail.com",
	#url
	description="Execute PHP scripts from within Python.",
	#long_description
	#classifiers
	#license
	packages=['pyphp'],
	package_dir={'pyphp': 'pyphp'},
	data_files=data_files.items(),
	ext_modules=[pyphp_ext]
)
