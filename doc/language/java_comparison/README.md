Jsonlang to Java
---------------

This little code example shows how a Jsonlang config using object-oriented features can be
transliterated (in a structure-preserving manner) to use the object-oriented features of Java.  Not
all Jsonlang programs can be converted to Java because Jsonlang has mixins and virtual inner classes.
Execution order is also different, as Jsonlang is lazy and Java is eager.  Likewise, Java programs
cannot all be converted to Jsonlang because they have mutable state and I/O.

However said all this, there is a significant overlap between the two languages.  This example fits
within that overlap.


Execute Jsonlang code
--------------------

```
jsonlang sub-template.jsonlang
```


Execute Java code
-----------------

We pipe into jsonlang - to pretty-print the output JSON.

```
javac *.java && java Test | jsonlang -
```


Structure of Java code
----------------------

The translation is as as follows:

* Everything is typed as Object, as Jsonlang is dynamically typed.  Everything is public, as is implicit in Jsonlang.
* Jsonlang arrays are Object[], and primitives are Boolean / Double / String.
* Jsonlang objects are singleton instances of named Java classes that extend JsonlangObject.
* Jsonlang fields become Java methods with no parameters (fields are virtual in Jsonlang).
* Jsonlang hidden field status is represented with a method nonHiddenFields which returns a set of
  field names.  Other fields are considered hidden.
* There are 2 liberties taken -- output strings are not escaped properly, and the JSON is printed on
  a single line instead of pretty-printed with indenting.

The Java code has a number of auxiliary framework functions to provide Jsonlang functionality that is
implicit in the Jsonlang language.

* The Test class chooses an object and manifests it to stdout.
* The JsonlangValue class implements manifestation, as a visitor over possible JSON values that
  builds JSON strings.  For objects, it iterates over the non-hidden fields and manifests each value
  by reflectively calling that function.


Reference JSON
--------------

For reference, the JSON that should be output is given in sub-template.json
