# Copyright 2016 LambdaStack All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Copyright 2015 Google Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
from setuptools import setup
from setuptools import Extension
from setuptools.command.build_ext import build_ext as BuildExt
from subprocess import Popen

DIR = os.path.abspath(os.path.dirname(__file__))
LIB_OBJECTS = [
    'core/desugarer.o',
    'core/formatter.o',
    'core/libjsonlang.o',
    'core/lexer.o',
    'core/parser.o',
    'core/static_analysis.o',
    'core/string_utils.o',
    'core/vm.o'
]

MODULE_SOURCES = ['python/_jsonlang.c']

def get_version():
    """
    Parses the version out of libjsonlang.h
    """
    with open(os.path.join(DIR, 'include/libjsonlang.h')) as f:
        for line in f:
            if '#define' in line and 'LIB_JSONLANG_VERSION' in line:
                v_code = line.partition('LIB_JSONLANG_VERSION')[2].strip('\n "')
                if v_code[0] == "v":
                    v_code = v_code[1:]
                return v_code

class BuildJsonlangExt(BuildExt):
    def run(self):
        p = Popen(['make'] + LIB_OBJECTS, cwd=DIR)
        p.wait()
        if p.returncode != 0:
            raise Exception('Could not build %s' % (', '.join(LIB_OBJECTS)))
        BuildExt.run(self)

jsonlang_ext = Extension(
    '_jsonlang',
    sources=MODULE_SOURCES,
    extra_objects=LIB_OBJECTS,
    include_dirs = ['include'],
    language='c++'
)

setup(name='jsonlang',
      url='https://jsonlang.org',
      description='Python bindings for Jsonlang - The data language ',
      author='Chris Jones',
      author_email='cjones@cloudm2.com',
      version=get_version(),
      cmdclass={
          'build_ext': BuildJsonlangExt,
      },
      ext_modules=[jsonlang_ext],
)
