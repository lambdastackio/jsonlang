/*
Copyright 2015 Google Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <cassert>
#include <cmath>

#include <memory>
#include <set>
#include <string>

// lambda - b
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdio.h>
#include <uuid/uuid.h>
// lambda - e

#include "desugarer.h"
#include "json.h"
#include "parser.h"
#include "state.h"
#include "static_analysis.h"
#include "string_utils.h"
#include "vm.h"

namespace {

/** Turn a path e.g. "/a/b/c" into a dir, e.g. "/a/b/".  If there is no path returns "".
 */
std::string dir_name(const std::string &path)
{
    size_t last_slash = path.rfind('/');
    if (last_slash != std::string::npos) {
        return path.substr(0, last_slash+1);
    }
    return "";
}

/** Stack frames.
 *
 * Of these, FRAME_CALL is the most special, as it is the only frame the stack
 * trace (for errors) displays.
 */
enum FrameKind {
    FRAME_APPLY_TARGET,  // e in e(...)
    FRAME_BINARY_LEFT,  // a in a + b
    FRAME_BINARY_RIGHT,  // b in a + b
    FRAME_BUILTIN_FILTER,  // When executing std.filter, used to hold intermediate state.
    FRAME_BUILTIN_FORCE_THUNKS,  // When forcing builtin args, holds intermediate state.
    FRAME_CALL,  // Used any time we have switched location in user code.
    FRAME_ERROR,  // e in error e
    FRAME_IF,  // e in if e then a else b
    FRAME_INDEX_TARGET,  // e in e[x]
    FRAME_INDEX_INDEX,  // e in x[e]
    FRAME_INVARIANTS,  // Caches the thunks that need to be executed one at a time.
    FRAME_LOCAL,  // Stores thunk bindings as we execute e in local ...; e
    FRAME_OBJECT,  // Stores intermediate state as we execute es in { [e]: ..., [e]: ... }
    FRAME_OBJECT_COMP_ARRAY,  // e in {f:a for x in e]
    FRAME_OBJECT_COMP_ELEMENT,  // Stores intermediate state when building object
    FRAME_STRING_CONCAT,  // Stores intermediate state while co-ercing objects
    FRAME_SUPER_INDEX,  // e in super[e]
    FRAME_UNARY,  // e in -e
};

/** A frame on the stack.
 *
 * Every time a subterm is evaluated, we first push a new stack frame to
 * store the continuation.
 *
 * The stack frame is a bit like a tagged union, except not as memory
 * efficient.  The set of member variables that are actually used depends on
 * the value of the member varaible kind.
 *
 * If the stack frame is of kind FRAME_CALL, then it counts towards the
 * maximum number of stack frames allowed.  Other stack frames are not
 * counted.  This is because FRAME_CALL exists where there is a branch in
 * the code, e.g. the forcing of a thunk, evaluation of a field, calling a
 * function, etc.
 *
 * The stack is used to mark objects during garbage
 * collection, so HeapObjects not referred to from the stack may be
 * prematurely collected.
 */
struct Frame {

    /** Tag (tagged union). */
    FrameKind kind;

    /** The code we were executing before. */
    const AST *ast;

    /** The location of the code we were executing before.
     *
     * location == ast->location when ast != nullptr
     */
    LocationRange location;

    /** Reuse this stack frame for the purpose of tail call optimization. */
    bool tailCall;

    /** Used for a variety of purposes. */
    Value val;

    /** Used for a variety of purposes. */
    Value val2;

    /** Used for a variety of purposes. */
    DesugaredObject::Fields::const_iterator fit;

    /** Used for a variety of purposes. */
    std::map<const Identifier *, HeapSimpleObject::Field> objectFields;

    /** Used for a variety of purposes. */
    unsigned elementId;

    /** Used for a variety of purposes. */
    std::map<const Identifier *, HeapThunk*> elements;

    /** Used for a variety of purposes. */
    std::vector<HeapThunk*> thunks;

    /** The context is used in error messages to attempt to find a reasonable name for the
     * object, function, or thunk value being executed.
     */
    HeapEntity *context;

    /** The lexically nearest object we are in, or nullptr.  Note
     * that this is not the same as context, because we could be inside a function,
     * inside an object and then context would be the function, but self would still point
     * to the object.
     */
    HeapObject *self;

    /** The "super" level of self.  Sometimes, we look upwards in the
     * inheritance tree, e.g. via an explicit use of super, or because a given field
     * has been inherited.  When evaluating a field from one of these super objects,
     * we need to bind self to the concrete object (so self must point
     * there) but uses of super should be resolved relative to the object whose
     * field we are evaluating.  Thus, we keep a second field for that.  This is
     * usually 0, unless we are evaluating a super object's field.
     */
    unsigned offset;

    /** A set of variables introduced at this point. */
    BindingFrame bindings;

    Frame(const FrameKind &kind, const AST *ast)
      : kind(kind), ast(ast), location(ast->location), tailCall(false), elementId(0),
        context(NULL), self(NULL), offset(0)
    {
        val.t = Value::NULL_TYPE;
        val2.t = Value::NULL_TYPE;
    }

    Frame(const FrameKind &kind, const LocationRange &location)
      : kind(kind), ast(nullptr), location(location), tailCall(false), elementId(0),
        context(NULL), self(NULL), offset(0)
    {
        val.t = Value::NULL_TYPE;
        val2.t = Value::NULL_TYPE;
    }

    /** Mark everything visible from this frame. */
    void mark(Heap &heap) const
    {
        heap.markFrom(val);
        heap.markFrom(val2);
        if (context) heap.markFrom(context);
        if (self) heap.markFrom(self);
        for (const auto &bind : bindings)
            heap.markFrom(bind.second);
        for (const auto &el : elements)
            heap.markFrom(el.second);
        for (const auto &th : thunks)
            heap.markFrom(th);
    }

    bool isCall(void) const
    {
        return kind == FRAME_CALL;
    }

};

/** The stack holds all the stack frames and manages the stack frame limit. */
class Stack {

    /** How many call frames are on the stack. */
    unsigned calls;

    /** How many call frames should be allowed before aborting the program. */
    unsigned limit;

    /** The stack frames. */
    std::vector<Frame> stack;

    public:

    Stack(unsigned limit)
      : calls(0), limit(limit)
    {
    }

    ~Stack(void) { }

    unsigned size(void)
    {
        return stack.size();
    }

    /** Search for the closest variable in scope that matches the given name. */
    HeapThunk *lookUpVar(const Identifier *id)
    {
        for (int i=stack.size()-1 ; i>=0 ; --i) {
            const auto &binds = stack[i].bindings;
            auto it = binds.find(id);
            if (it != binds.end()) {
                return it->second;
            }
            if (stack[i].isCall()) break;
        }
        return nullptr;
    }

    /** Mark everything visible from the stack (any frame). */
    void mark(Heap &heap)
    {
        for (const auto &f : stack) {
            f.mark(heap);
        }
    }

    Frame &top(void)
    {
        return stack.back();
    }

    const Frame &top(void) const
    {
        return stack.back();
    }

    void pop(void)
    {
        if (top().isCall()) calls--;
        stack.pop_back();
    }

    /** Attempt to find a name for a given heap entity.  This may not be possible, but we try
     * reasonably hard.  We look in the bindings for a variable in the closest scope that
     * happens to point at the entity in question.  Otherwise, the best we can do is use its
     * type.
     */
    std::string getName(unsigned from_here, const HeapEntity *e)
    {
        std::string name;
        for (int i=from_here-1 ; i>=0; --i) {
            const auto &f = stack[i];
            for (const auto &pair : f.bindings) {
                HeapThunk *thunk = pair.second;
                if (!thunk->filled) continue;
                if (!thunk->content.isHeap()) continue;
                if (e != thunk->content.v.h) continue;
                name = encode_utf8(pair.first->name);
            }
            // Do not go into the next call frame, keep local reasoning.
            if (f.isCall()) break;
        }

        if (name == "") name = "anonymous";
        if (dynamic_cast<const HeapObject*>(e)) {
            return "object <" + name + ">";
        } else if (auto *thunk = dynamic_cast<const HeapThunk*>(e)) {
            if (thunk->name == nullptr) {
                return "";  // Argument of builtin, or root (since top level functions).
            } else {
                return "thunk <" + encode_utf8(thunk->name->name) + ">";
            }
        } else {
            const auto *func = static_cast<const HeapClosure *>(e);
            if (func->body == nullptr) {
                return "builtin function <" + func->builtinName + ">";
            }
            return "function <" + name + ">";
        }
    }

    /** Dump the stack.
     *
     * This is useful to help debug the VM in gdb.  It is virtual to stop it
     * being removed by the compiler.
     */
    virtual void dump(void)
    {
        for (unsigned i=0 ; i<stack.size() ; ++i) {
            std::cout << "stack[" << i << "] = " << stack[i].location
                      << " (" << stack[i].kind << ")"
                      << std::endl;
        }
        std::cout << std::endl;
    }

    /** Creates the error object for throwing, and also populates it with the stack trace.
     */
    RuntimeError makeError(const LocationRange &loc, const std::string &msg)
    {
        std::vector<TraceFrame> stack_trace;
        stack_trace.push_back(TraceFrame(loc));
        for (int i=stack.size()-1 ; i>=0 ; --i) {
            const auto &f = stack[i];
            if (f.isCall()) {
                if (f.context != nullptr) {
                    // Give the last line a name.
                    stack_trace[stack_trace.size()-1].name = getName(i, f.context);
                }
                if (f.location.isSet() || f.location.file.length() > 0)
                    stack_trace.push_back(TraceFrame(f.location));
            }
        }
        return RuntimeError(stack_trace, msg);
    }

    /** New (non-call) frame. */
    template <class... Args> void newFrame(Args... args)
    {
        stack.emplace_back(args...);
    }

    /** If there is a tailstrict annotated frame followed by some locals, pop them all. */
    void tailCallTrimStack (void)
    {
        for (int i=stack.size()-1 ; i>=0 ; --i) {
            switch (stack[i].kind) {
                case FRAME_CALL: {
                    if (!stack[i].tailCall || stack[i].thunks.size() > 0) {
                        return;
                    }
                    // Remove all stack frames including this one.
                    while (stack.size() > unsigned(i)) stack.pop_back();
                    calls--;
                    return;
                } break;

                case FRAME_LOCAL: break;

                default: return;
            }
        }
    }

    /** New call frame. */
    void newCall(const LocationRange &loc, HeapEntity *context, HeapObject *self,
                 unsigned offset, const BindingFrame &up_values)
    {
        tailCallTrimStack();
        if (calls >= limit) {
            throw makeError(loc, "Max stack frames exceeded.");
        }
        stack.emplace_back(FRAME_CALL, loc);
        calls++;
        top().context = context;
        top().self = self;
        top().offset = offset;
        top().bindings = up_values;
        top().tailCall = false;

        #ifndef NDEBUG
        for (const auto &bind : up_values) {
            if (bind.second == nullptr) {
                std::cerr << "INTERNAL ERROR: No binding for variable "
                          << encode_utf8(bind.first->name) << std::endl;
                std::abort();
            }
        }
        #endif
    }

    /** Look up the stack to find the self binding. */
    void getSelfBinding(HeapObject *&self, unsigned &offset)
    {
        self = nullptr;
        offset = 0;
        for (int i=stack.size() - 1 ; i>=0 ; --i) {
            if (stack[i].isCall()) {
                self = stack[i].self;
                offset = stack[i].offset;
                return;
            }
        }
    }

    /** Look up the stack to see if we're running assertions for this object. */
    bool alreadyExecutingInvariants(HeapObject *self)
    {
        for (int i=stack.size() - 1 ; i>=0 ; --i) {
            if (stack[i].kind == FRAME_INVARIANTS) {
                if (stack[i].self == self) return true;
            }
        }
        return false;
    }
};

/** Typedef to save some typing. */
typedef std::map<std::string, VmExt> ExtMap;

/** Typedef to save some typing. */
typedef std::map<std::string, std::string> StrMap;


class Interpreter;

typedef const AST *(Interpreter::*BuiltinFunc)(const LocationRange &loc,
                                               const std::vector<Value> &args);

/** Holds the intermediate state during execution and implements the necessary functions to
 * implement the semantics of the language.
 *
 * The garbage collector used is a simple stop-the-world mark and sweep collector.  It runs upon
 * memory allocation if the heap is large enough and has grown enough since the last collection.
 * All reachable entities have their mark field incremented.  Then all entities with the old
 * mark are removed from the heap.
 */
class Interpreter {

    /** The heap. */
    Heap heap;

    /** The value last computed. */
    Value scratch;

    /** The stack. */
    Stack stack;

    /** Used to create ASTs if needed.
     *
     * This is used at import time, and in a few other cases.
     */
    Allocator *alloc;

    /** Used to "name" thunks created on the inside of an array. */
    const Identifier *idArrayElement;

    /** Used to "name" thunks created to execute invariants. */
    const Identifier *idInvariant;

    /** Used to "name" thunks created to convert JSON to Jsonlang objects. */
    const Identifier *idJsonObjVar;

    /** Used to refer to idJsonObjVar. */
    const AST *jsonObjVar;

    struct ImportCacheValue {
        std::string foundHere;
        std::string content;
    };

    /** Cache for imported Jsonlang files. */
    std::map<std::pair<std::string, String>,
             const ImportCacheValue *> cachedImports;

    /** External variables for std.extVar. */
    ExtMap externalVars;

    /** The callback used for loading imported files. */
    VmNativeCallbackMap nativeCallbacks;

    /** The callback used for loading imported files. */
    JsonlangImportCallback *importCallback;

    /** User context pointer for the import callback. */
    void *importCallbackContext;

    /** Builtin functions by name. */
    typedef std::map<std::string, BuiltinFunc> BuiltinMap;
    BuiltinMap builtins;

    RuntimeError makeError(const LocationRange &loc, const std::string &msg)
    {
        return stack.makeError(loc, msg);
    }

    /** Create an object on the heap, maybe collect garbage.
     * \param T Something under HeapEntity
     * \returns The new object
     */
    template <class T, class... Args> T* makeHeap(Args&&... args)
    {
        T *r = heap.makeEntity<T, Args...>(std::forward<Args>(args)...);
        if (heap.checkHeap()) {  // Do a GC cycle?
            // Avoid the object we just made being collected.
            heap.markFrom(r);

            // Mark from the stack.
            stack.mark(heap);

            // Mark from the scratch register
            heap.markFrom(scratch);

            // Delete unreachable objects.
            heap.sweep();
        }
        return r;
    }

    Value makeBoolean(bool v)
    {
        Value r;
        r.t = Value::BOOLEAN;
        r.v.b = v;
        return r;
    }

    Value makeDouble(double v)
    {
        Value r;
        r.t = Value::DOUBLE;
        r.v.d = v;
        return r;
    }

    Value makeDoubleCheck(const LocationRange &loc, double v)
    {
        if (std::isnan(v)) {
            throw makeError(loc, "Not a number");
        }
        if (std::isinf(v)) {
            throw makeError(loc, "Overflow");
        }
        return makeDouble(v);
    }

    Value makeNull(void)
    {
        Value r;
        r.t = Value::NULL_TYPE;
        return r;
    }

    Value makeArray(const std::vector<HeapThunk*> &v)
    {
        Value r;
        r.t = Value::ARRAY;
        r.v.h = makeHeap<HeapArray>(v);
        return r;
    }

    Value makeClosure(const BindingFrame &env,
                       HeapObject *self,
                       unsigned offset,
                       const HeapClosure::Params &params,
                       AST *body)
    {
        Value r;
        r.t = Value::FUNCTION;
        r.v.h = makeHeap<HeapClosure>(env, self, offset, params, body, "");
        return r;
    }

    Value makeNativeBuiltin(const std::string &name, const std::vector<std::string> &params)
    {
        HeapClosure::Params hc_params;
        for (const auto &p : params) {
            hc_params.emplace_back(alloc->makeIdentifier(decode_utf8(p)), nullptr);
        }
        return makeBuiltin(name, hc_params);
    }

    Value makeBuiltin(const std::string &name, const HeapClosure::Params &params)
    {
        AST *body = nullptr;
        Value r;
        r.t = Value::FUNCTION;
        r.v.h = makeHeap<HeapClosure>(BindingFrame(), nullptr, 0, params, body, name);
        return r;
    }

    template <class T, class... Args> Value makeObject(Args... args)
    {
        Value r;
        r.t = Value::OBJECT;
        r.v.h = makeHeap<T>(args...);
        return r;
    }

    Value makeString(const String &v)
    {
        Value r;
        r.t = Value::STRING;
        r.v.h = makeHeap<HeapString>(v);
        return r;
    }

    /** Auxiliary function of objectIndex.
     *
     * Traverse the object's tree from right to left, looking for an object
     * with the given field.  Call with offset initially set to 0.
     *
     * \param f The field we're looking for.
     * \param start_from Step over this many leaves first.
     * \param counter Return the level of "super" that contained the field.
     * \returns The first object with the field, or nullptr if it could not be found.
     */
    HeapLeafObject *findObject(const Identifier *f, HeapObject *curr,
                               unsigned start_from, unsigned &counter)
    {
        if (auto *ext = dynamic_cast<HeapExtendedObject*>(curr)) {
            auto *r = findObject(f, ext->right, start_from, counter);
            if (r) return r;
            auto *l = findObject(f, ext->left, start_from, counter);
            if (l) return l;
        } else {
            if (counter >= start_from) {
                if (auto *simp = dynamic_cast<HeapSimpleObject*>(curr)) {
                    auto it = simp->fields.find(f);
                    if (it != simp->fields.end()) {
                        return simp;
                    }
                } else if (auto *comp = dynamic_cast<HeapComprehensionObject*>(curr)) {
                    auto it = comp->compValues.find(f);
                    if (it != comp->compValues.end()) {
                        return comp;
                    }
                }
            }
            counter++;
        }
        return nullptr;
    }

    typedef std::map<const Identifier*, ObjectField::Hide> IdHideMap;

    /** Auxiliary function.
     */
    IdHideMap objectFieldsAux(const HeapObject *obj_)
    {
        IdHideMap r;
        if (auto *obj = dynamic_cast<const HeapSimpleObject*>(obj_)) {
            for (const auto &f : obj->fields) {
                r[f.first] = f.second.hide;
            }

        } else if (auto *obj = dynamic_cast<const HeapExtendedObject*>(obj_)) {
            r = objectFieldsAux(obj->right);
            for (const auto &pair : objectFieldsAux(obj->left)) {
                auto it = r.find(pair.first);
                if (it == r.end()) {
                    // First time it is seen
                    r[pair.first] = pair.second;
                } else if (it->second == ObjectField::INHERIT) {
                    // Seen before, but with inherited visibility so use new visibility
                    r[pair.first] = pair.second;
                }
            }

        } else if (auto *obj = dynamic_cast<const HeapComprehensionObject*>(obj_)) {
            for (const auto &f : obj->compValues)
                r[f.first] = ObjectField::VISIBLE;
        }
        return r;
    }

    /** Auxiliary function.
     */
    std::set<const Identifier*> objectFields(const HeapObject *obj_, bool manifesting)
    {
        std::set<const Identifier*> r;
        for (const auto &pair : objectFieldsAux(obj_)) {
            if (!manifesting || pair.second != ObjectField::HIDDEN) r.insert(pair.first);
        }
        return r;
    }

    // lambda - b
    /** Exec an executable or script that produces a Jsonlang or Json file.
     *
     * If the file has already been imported (net result of exec), then use that version.  This maintains
     * referential transparency in the case of writes to disk during execution. Could have added logic to import
     * funtion instead but did it exec so as to make it clear and also add additional value at some point.
     *
     * \param loc Location of the exec statement.
     * \param file Path to the filename.
     */
    AST *exec(const LocationRange &loc, const LiteralString *cmdLiteralString)  //const std::string &cmd)
    {
        //std::string dir = dir_name(loc.file);
        //if (dir.length() > 0)
        //    abs_file = dir + abs_file;

        // Cmd should be in $PATH
        std::string cmd = (const char *)cmdLiteralString->value.c_str();
        std::string results = exec_cmd(cmd.c_str());

        // Now that we have the jsonlang result the simplest thing is to write to the
        std::ofstream jsonlang_file;
        std::string uuid = uuidgen();
        std::size_t pos = cmd.find(" ");
        std::string new_cmd = cmd.substr(0, pos);
        std::string new_file = new_cmd + "-" + uuid + ".jsonlang";
        jsonlang_file.open(new_file, std::ios::trunc);
        if (results.length() > 0) {
            if ( results['{'] == 0) {
                // Assume json data so process as normal
                jsonlang_file << results;
            } else {
                std::string new_output = "{\"" + new_cmd + "\": [";
                std::istringstream iss(results);
                for (std::string line; std::getline(iss, line);) {
                    new_output += line_escape(line) + ", ";
                }
                new_output += "]}";
                jsonlang_file << new_output;
            }
        } else {
            jsonlang_file << "{}";
        }
        jsonlang_file.close();

        auto *expr = import(loc, (LiteralString *)new_file.c_str());

        remove(new_file.c_str());
        return expr;
    }

    std::string line_escape(const std::string &str)
    {
        std::stringstream ss;
        ss << '\"';
        for (std::size_t i=0 ; i<str.length() ; ++i) {
            char c = str[i];
            switch (c) {
                case '"': ss << "\\\""; break;
                case '\\': ss << "\\\\"; break;
                case '\b': ss << "\\b"; break;
                case '\f': ss << "\\f"; break;
                //case '\n': ss << "\\n"; break;
                case '\n': ss << ""; break;
                case '\r': ss << "\\r"; break;
                case '\t': ss << "\\t"; break;
                case '\0': ss << "\\u0000"; break;
                default: {
                    //if (c < 0x20 || c > 0x7e) {
                        ////Unprintable, use \u
                        //ss << "\\u" << std::hex << std::setfill('0') << std::setw(4)
                        //   << unsigned((unsigned char)(c));
                    //} else {
                        // Printable, write verbatim
                        ss << c;
                    //}
                }
            }
        }
        ss << '\"';
        return ss.str();
    }

    std::string uuidgen()
    {
        uuid_t id;
        uuid_generate(id);
        char *uniq = new char[100];
        char buff[100];
        uuid_unparse(id, uniq);
        sprintf(buff, "%s", uniq);
        std::string new_file = buff;
        free(uniq);
        return new_file;
    }

    /** exec_import - Used by import to execute a non json/jsonlang/http value.
    * \param loc Location of the exec statement.
    * \param file Path to the filename.
    */
    std::string *http_import(const LocationRange &loc, const std::string &file)
    {
        // May need to add to libjsonlang so as to be supported in other languages.
        // Could maybe check for execution writes
        std::string cmd = file;
        std::string dir = dir_name(loc.file);
        if (file.find("curl ") == std::string::npos)
            cmd = "curl --silent " + cmd;

        std::string results = exec_cmd(cmd.c_str());

        // Now that we have the jsonlang result the simplest thing is to write to the
        std::ofstream jsonlang_file;

        uuid_t id;
        uuid_generate(id);
        char *uniq = new char[100];
        char buff[180];
        uuid_unparse(id, uniq);
        sprintf(buff, "%s%s", uniq, ".jsonlang");
        std::string new_file = buff;
        free(uniq);

        jsonlang_file.open(dir + new_file.c_str(), std::ios::trunc);
        if (results.length() > 0) {
            jsonlang_file << results;
        } else {
            jsonlang_file << "{}";
        }
        jsonlang_file.close();

        return new std::string(new_file);
    }

    /** exec_import - Used by import to execute a non json/jsonlang/http value.
    * \param loc Location of the exec statement.
    * \param file Path to the filename.
    */
    std::string *exec_import(const LocationRange &loc, const std::string &file)
    {
        // May need to add to libjsonlang so as to be supported in other languages.
        // Could maybe check for execution writes
        std::string dir = dir_name(loc.file);
        std::string abs_file = file;
        if (dir.length() > 0)
            abs_file = dir + abs_file;

        std::string results = exec_cmd(abs_file.c_str());

        // Now that we have the jsonlang result the simplest thing is to write to the
        std::ofstream jsonlang_file;
        jsonlang_file.open(abs_file+".jsonlang", std::ios::trunc);
        if (results.length() > 0) {
            jsonlang_file << results;
        } else {
            jsonlang_file << "{}";
        }
        jsonlang_file.close();

        std::string new_file = file + ".jsonlang";
        return new std::string(new_file);
    }

    /** exec_cmd is used internally to support the exec call. Could change popen to _popen for windows.
    * \param cmd The command to be executed.
    */
    std::string exec_cmd(const char* cmd) {
        FILE* pipe = popen(cmd, "r");
        if (!pipe)
            return "";

        char buffer[4096];
        std::string result = "";
        while(!feof(pipe)) {
          if(fgets(buffer, 4096, pipe) != NULL)
            result += buffer;
        }
        pclose(pipe);
        return result;
    }
    // lambda - e

    /** Import another Jsonlang file.
     *
     * If the file has already been imported, then use that version.  This maintains
     * referential transparency in the case of writes to disk during execution.
     *
     * \param loc Location of the import statement.
     * \param file Path to the filename.
     */
    AST *import(const LocationRange &loc, const LiteralString *file)
    {
        const ImportCacheValue *input = importString(loc, file);
        Tokens tokens = jsonlang_lex(input->foundHere, input->content.c_str());
        AST *expr = jsonlang_parse(alloc, tokens);
        jsonlang_desugar(alloc, expr, nullptr);
        jsonlang_static_analysis(expr);
        return expr;
    }

    /** Import a file as a string.
     *
     * If the file has already been imported, then use that version.  This maintains
     * referential transparency in the case of writes to disk during execution.
     *
     * \param loc Location of the import statement.
     * \param file Path to the filename.
     * \param found_here If non-null, used to store the actual path of the file
     */
    const ImportCacheValue *importString(const LocationRange &loc, const LiteralString *file)
    {
        std::string dir = dir_name(loc.file);

        const String &path = file->value;

        std::pair<std::string, String> key(dir, path);
        const ImportCacheValue *cached_value = cachedImports[key];
        if (cached_value != nullptr)
            return cached_value;

        int success = 0;
        char *found_here_cptr;
        char *content =
            importCallback(importCallbackContext, dir.c_str(), encode_utf8(path).c_str(),
                           &found_here_cptr, &success);

        std::string input(content);
        ::free(content);

        if (!success) {
            std::string msg = "Couldn't open import \"" + encode_utf8(path) + "\": ";
            msg += input;
            throw makeError(loc, msg);
        }

        auto *input_ptr = new ImportCacheValue();
        input_ptr->foundHere = found_here_cptr;
        input_ptr->content = input;
        ::free(found_here_cptr);
        cachedImports[key] = input_ptr;
        return input_ptr;
    }

    /** Capture the required variables from the environment. */
    BindingFrame capture(const std::vector<const Identifier*> &free_vars)
    {
        BindingFrame env;
        for (auto fv : free_vars) {
            auto *th = stack.lookUpVar(fv);
            env[fv] = th;
        }
        return env;
    }

    /** Count the number of leaves in the tree.
     *
     * \param obj The root of the tree.
     * \param counter Initialize to 0, returns the number of leaves.
     */
    unsigned countLeaves(HeapObject *obj)
    {
        if (auto *ext = dynamic_cast<HeapExtendedObject*>(obj)) {
            return countLeaves(ext->left) + countLeaves(ext->right);
        } else {
            return 1;
        }
    }

    public:

    /** Create a new interpreter.
     *
     * \param loc The location range of the file to be executed.
     */
    Interpreter(
        Allocator *alloc,
        const ExtMap &ext_vars,
        unsigned max_stack,
        double gc_min_objects,
        double gc_growth_trigger,
        const VmNativeCallbackMap &native_callbacks,
        JsonlangImportCallback *import_callback,
        void *import_callback_context)

      : heap(gc_min_objects, gc_growth_trigger),
        stack(max_stack),
        alloc(alloc),
        idArrayElement(alloc->makeIdentifier(U"array_element")),
        idInvariant(alloc->makeIdentifier(U"object_assert")),
        idJsonObjVar(alloc->makeIdentifier(U"_")),
        jsonObjVar(alloc->make<Var>(LocationRange(), Fodder{}, idJsonObjVar)),
        externalVars(ext_vars),
        nativeCallbacks(native_callbacks),
        importCallback(import_callback),
        importCallbackContext(import_callback_context)
    {
        scratch = makeNull();
        builtins["makeArray"] = &Interpreter::builtinMakeArray;
        builtins["pow"] = &Interpreter::builtinPow;
        builtins["floor"] = &Interpreter::builtinFloor;
        builtins["ceil"] = &Interpreter::builtinCeil;
        builtins["sqrt"] = &Interpreter::builtinSqrt;
        builtins["sin"] = &Interpreter::builtinSin;
        builtins["cos"] = &Interpreter::builtinCos;
        builtins["tan"] = &Interpreter::builtinTan;
        builtins["asin"] = &Interpreter::builtinAsin;
        builtins["acos"] = &Interpreter::builtinAcos;
        builtins["atan"] = &Interpreter::builtinAtan;
        builtins["type"] = &Interpreter::builtinType;
        builtins["filter"] = &Interpreter::builtinFilter;
        builtins["objectHasEx"] = &Interpreter::builtinObjectHasEx;
        builtins["length"] = &Interpreter::builtinLength;
        builtins["objectFieldsEx"] = &Interpreter::builtinObjectFieldsEx;
        builtins["codepoint"] = &Interpreter::builtinCodepoint;
        builtins["char"] = &Interpreter::builtinChar;
        builtins["log"] = &Interpreter::builtinLog;
        builtins["exp"] = &Interpreter::builtinExp;
        builtins["mantissa"] = &Interpreter::builtinMantissa;
        builtins["exponent"] = &Interpreter::builtinExponent;
        builtins["modulo"] = &Interpreter::builtinModulo;
        builtins["extVar"] = &Interpreter::builtinExtVar;
        builtins["primitiveEquals"] = &Interpreter::builtinPrimitiveEquals;
        builtins["native"] = &Interpreter::builtinNative;
    }

    /** Clean up the heap, stack, stash, and builtin function ASTs. */
    ~Interpreter()
    {
        for (const auto &pair : cachedImports) {
            delete pair.second;
        }
    }

    const Value &getScratchRegister(void)
    {
        return scratch;
    }

    void setScratchRegister(const Value &v)
    {
        scratch = v;
    }


    /** Raise an error if the arguments aren't the expected types. */
    void validateBuiltinArgs(const LocationRange &loc,
                             const std::string &name,
                             const std::vector<Value> &args,
                             const std::vector<Value::Type> params)
    {
        if (args.size() == params.size()) {
            for (unsigned i=0 ; i<args.size() ; ++i) {
                if (args[i].t != params[i]) goto bad;
            }
            return;
        }
        bad:;
        std::stringstream ss;
        ss << "Builtin function " + name + " expected (";
        const char *prefix = "";
        for (auto p : params) {
            ss << prefix << type_str(p);
            prefix = ", ";
        }
        ss << ") but got (";
        prefix = "";
        for (auto a : args) {
            ss << prefix << type_str(a);
            prefix = ", ";
        }
        ss << ")";
        throw makeError(loc, ss.str());
    }

    const AST *builtinMakeArray(const LocationRange &loc, const std::vector<Value> &args)
    {
        Frame &f = stack.top();
        validateBuiltinArgs(loc, "makeArray", args,
                            {Value::DOUBLE, Value::FUNCTION});
        long sz = long(args[0].v.d);
        if (sz < 0) {
            std::stringstream ss;
            ss << "makeArray requires size >= 0, got " << sz;
            throw makeError(loc, ss.str());
        }
        auto *func = static_cast<const HeapClosure*>(args[1].v.h);
        std::vector<HeapThunk*> elements;
        if (func->params.size() != 1) {
            std::stringstream ss;
            ss << "makeArray function must take 1 param, got: " << func->params.size();
            throw makeError(loc, ss.str());
        }
        elements.resize(sz);
        for (long i=0 ; i<sz ; ++i) {
            auto *th = makeHeap<HeapThunk>(idArrayElement, func->self, func->offset, func->body);
            // The next line stops the new thunks from being GCed.
            f.thunks.push_back(th);
            th->upValues = func->upValues;

            auto *el = makeHeap<HeapThunk>(func->params[0].id, nullptr, 0, nullptr);
            el->fill(makeDouble(i));  // i guaranteed not to be inf/NaN
            th->upValues[func->params[0].id] = el;
            elements[i] = th;
        }
        scratch = makeArray(elements);
        return nullptr;
    }

    const AST *builtinPow(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "pow", args, {Value::DOUBLE, Value::DOUBLE});
        scratch = makeDoubleCheck(loc, std::pow(args[0].v.d, args[1].v.d));
        return nullptr;
    }

    const AST *builtinFloor(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "floor", args, {Value::DOUBLE});
        scratch = makeDoubleCheck(loc, std::floor(args[0].v.d));
        return nullptr;
    }

    const AST *builtinCeil(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "ceil", args, {Value::DOUBLE});
        scratch = makeDoubleCheck(loc, std::ceil(args[0].v.d));
        return nullptr;
    }

    const AST *builtinSqrt(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "sqrt", args, {Value::DOUBLE});
        scratch = makeDoubleCheck(loc, std::sqrt(args[0].v.d));
        return nullptr;
    }

    const AST *builtinSin(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "sin", args, {Value::DOUBLE});
        scratch = makeDoubleCheck(loc, std::sin(args[0].v.d));
        return nullptr;
    }

    const AST *builtinCos(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "cos", args, {Value::DOUBLE});
        scratch = makeDoubleCheck(loc, std::cos(args[0].v.d));
        return nullptr;
    }

    const AST *builtinTan(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "tan", args, {Value::DOUBLE});
        scratch = makeDoubleCheck(loc, std::tan(args[0].v.d));
        return nullptr;
    }

    const AST *builtinAsin(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "asin", args, {Value::DOUBLE});
        scratch = makeDoubleCheck(loc, std::asin(args[0].v.d));
        return nullptr;
    }

    const AST *builtinAcos(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "acos", args, {Value::DOUBLE});
        scratch = makeDoubleCheck(loc, std::acos(args[0].v.d));
        return nullptr;
    }

    const AST *builtinAtan(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "atan", args, {Value::DOUBLE});
        scratch = makeDoubleCheck(loc, std::atan(args[0].v.d));
        return nullptr;
    }

    const AST *builtinType(const LocationRange &, const std::vector<Value> &args)
    {
        switch (args[0].t) {
            case Value::NULL_TYPE:
            scratch = makeString(U"null");
            return nullptr;

            case Value::BOOLEAN:
            scratch = makeString(U"boolean");
            return nullptr;

            case Value::DOUBLE:
            scratch = makeString(U"number");
            return nullptr;

            case Value::ARRAY:
            scratch = makeString(U"array");
            return nullptr;

            case Value::FUNCTION:
            scratch = makeString(U"function");
            return nullptr;

            case Value::OBJECT:
            scratch = makeString(U"object");
            return nullptr;

            case Value::STRING:
            scratch = makeString(U"string");
            return nullptr;
        }
        return nullptr;  // Quiet, compiler.
    }

    const AST *builtinFilter(const LocationRange &loc, const std::vector<Value> &args)
    {
        Frame &f = stack.top();
        validateBuiltinArgs(loc, "filter", args, {Value::FUNCTION, Value::ARRAY});
        auto *func = static_cast<HeapClosure*>(args[0].v.h);
        auto *arr = static_cast<HeapArray*>(args[1].v.h);
        if (func->params.size() != 1) {
            throw makeError(loc, "filter function takes 1 parameter.");
        }
        if (arr->elements.size() == 0) {
            scratch = makeArray({});
        } else {
            f.kind = FRAME_BUILTIN_FILTER;
            f.val = args[0];
            f.val2 = args[1];
            f.thunks.clear();
            f.elementId = 0;

            auto *thunk = arr->elements[f.elementId];
            BindingFrame bindings = func->upValues;
            bindings[func->params[0].id] = thunk;
            stack.newCall(loc, func, func->self, func->offset, bindings);
            return func->body;
        }
        return nullptr;
    }

    const AST *builtinObjectHasEx(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "objectHasEx", args,
                            {Value::OBJECT, Value::STRING,
                             Value::BOOLEAN});
        const auto *obj = static_cast<const HeapObject*>(args[0].v.h);
        const auto *str = static_cast<const HeapString*>(args[1].v.h);
        bool include_hidden = args[2].v.b;
        bool found = false;
        for (const auto &field : objectFields(obj, !include_hidden)) {
            if (field->name == str->value) {
                found = true;
                break;
            }
        }
        scratch = makeBoolean(found);
        return nullptr;
    }

    const AST *builtinLength(const LocationRange &loc, const std::vector<Value> &args)
    {
        if (args.size() != 1) {
            throw makeError(loc, "length takes 1 parameter.");
        }
        HeapEntity *e = args[0].v.h;
        switch (args[0].t) {
            case Value::OBJECT: {
                auto fields = objectFields(static_cast<HeapObject*>(e), true);
                scratch = makeDouble(fields.size());
            } break;

            case Value::ARRAY:
            scratch = makeDouble(static_cast<HeapArray*>(e)->elements.size());
            break;

            case Value::STRING:
            scratch = makeDouble(static_cast<HeapString*>(e)->value.length());
            break;

            case Value::FUNCTION:
            scratch = makeDouble(static_cast<HeapClosure*>(e)->params.size());
            break;

            default:
            throw makeError(loc,
                            "length operates on strings, objects, "
                            "and arrays, got " + type_str(args[0]));
        }
        return nullptr;
    }

    const AST *builtinObjectFieldsEx(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "objectFieldsEx", args,
                            {Value::OBJECT, Value::BOOLEAN});
        const auto *obj = static_cast<HeapObject*>(args[0].v.h);
        bool include_hidden = args[1].v.b;
        // Stash in a set first to sort them.
        std::set<String> fields;
        for (const auto &field : objectFields(obj, !include_hidden)) {
            fields.insert(field->name);
        }
        scratch = makeArray({});
        auto &elements = static_cast<HeapArray*>(scratch.v.h)->elements;
        for (const auto &field : fields) {
            auto *th = makeHeap<HeapThunk>(idArrayElement, nullptr, 0, nullptr);
            elements.push_back(th);
            th->fill(makeString(field));
        }
        return nullptr;
    }

    const AST *builtinCodepoint(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "codepoint", args, {Value::STRING});
        const String &str =
            static_cast<HeapString*>(args[0].v.h)->value;
        if (str.length() != 1) {
            std::stringstream ss;
            ss << "codepoint takes a string of length 1, got length "
               << str.length();
            throw makeError(loc, ss.str());
        }
        char32_t c = static_cast<HeapString*>(args[0].v.h)->value[0];
        scratch = makeDouble((unsigned long)(c));
        return nullptr;
    }

    const AST *builtinChar(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "char", args, {Value::DOUBLE});
        long l = long(args[0].v.d);
        if (l < 0) {
            std::stringstream ss;
            ss << "Codepoints must be >= 0, got " << l;
            throw makeError(loc, ss.str());
        }
        if (l >= JSONLANG_CODEPOINT_MAX) {
            std::stringstream ss;
            ss << "Invalid unicode codepoint, got " << l;
            throw makeError(loc, ss.str());
        }
        char32_t c = l;
        scratch = makeString(String(&c, 1));
        return nullptr;
    }

    const AST *builtinLog(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "log", args, {Value::DOUBLE});
        scratch = makeDoubleCheck(loc, std::log(args[0].v.d));
        return nullptr;
    }

    const AST *builtinExp(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "exp", args, {Value::DOUBLE});
        scratch = makeDoubleCheck(loc, std::exp(args[0].v.d));
        return nullptr;
    }

    const AST *builtinMantissa(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "mantissa", args, {Value::DOUBLE});
        int exp;
        double m = std::frexp(args[0].v.d, &exp);
        scratch = makeDoubleCheck(loc, m);
        return nullptr;
    }

    const AST *builtinExponent(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "exponent", args, {Value::DOUBLE});
        int exp;
        std::frexp(args[0].v.d, &exp);
        scratch = makeDoubleCheck(loc, exp);
        return nullptr;
    }

    const AST *builtinModulo(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "modulo", args, {Value::DOUBLE, Value::DOUBLE});
        double a = args[0].v.d;
        double b = args[1].v.d;
        if (b == 0)
            throw makeError(loc, "Division by zero.");
        scratch = makeDoubleCheck(loc, std::fmod(a, b));
        return nullptr;
    }

    const AST *builtinExtVar(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "extVar", args, {Value::STRING});
        const String &var =
            static_cast<HeapString*>(args[0].v.h)->value;
        std::string var8 = encode_utf8(var);
        auto it = externalVars.find(var8);
        if (it == externalVars.end()) {
            std::string msg = "Undefined external variable: " + var8;
            throw makeError(loc, msg);
        }
        const VmExt &ext = it->second;
        if (ext.isCode) {
            std::string filename = "<extvar:" + var8 + ">";
            Tokens tokens = jsonlang_lex(filename, ext.data.c_str());
            AST *expr = jsonlang_parse(alloc, tokens);
            jsonlang_desugar(alloc, expr, nullptr);
            jsonlang_static_analysis(expr);
            stack.pop();
            return expr;
        } else {
            scratch = makeString(decode_utf8(ext.data));
            return nullptr;
        }
    }

    const AST *builtinPrimitiveEquals(const LocationRange &loc, const std::vector<Value> &args)
    {
        if (args.size() != 2) {
            std::stringstream ss;
            ss << "primitiveEquals takes 2 parameters, got " << args.size();
            throw makeError(loc, ss.str());
        }
        if (args[0].t != args[1].t) {
            scratch = makeBoolean(false);
            return nullptr;
        }
        bool r;
        switch (args[0].t) {
            case Value::BOOLEAN:
            r = args[0].v.b == args[1].v.b;
            break;

            case Value::DOUBLE:
            r = args[0].v.d == args[1].v.d;
            break;

            case Value::STRING:
            r = static_cast<HeapString*>(args[0].v.h)->value
              == static_cast<HeapString*>(args[1].v.h)->value;
            break;

            case Value::NULL_TYPE:
            r = true;
            break;

            case Value::FUNCTION:
            throw makeError(loc, "Cannot test equality of functions");
            break;

            default:
            throw makeError(loc,
                            "primitiveEquals operates on primitive "
                            "types, got " + type_str(args[0]));
        }
        scratch = makeBoolean(r);
        return nullptr;
    }

    const AST *builtinNative(const LocationRange &loc, const std::vector<Value> &args)
    {
        validateBuiltinArgs(loc, "native", args, {Value::STRING});

        std::string builtin_name = encode_utf8(static_cast<HeapString*>(args[0].v.h)->value);

        VmNativeCallbackMap::const_iterator nit = nativeCallbacks.find(builtin_name);
        if (nit == nativeCallbacks.end()) {
            throw makeError(loc, "Unrecognized native function name: " + builtin_name);
        }

        const VmNativeCallback &cb = nit->second;
        scratch = makeNativeBuiltin(builtin_name, cb.params);
        return nullptr;
    }

    void jsonToHeap(const std::unique_ptr<JsonlangJsonValue> &v, Value &attach)
    {
        // In order to not anger the garbage collector, assign to attach immediately after
        // making the heap object.
        switch (v->kind) {
            case JsonlangJsonValue::STRING:
            attach = makeString(decode_utf8(v->string));
            break;

            case JsonlangJsonValue::BOOL:
            attach = makeBoolean(v->number != 0.0);
            break;

            case JsonlangJsonValue::NUMBER:
            attach = makeDouble(v->number);
            break;

            case JsonlangJsonValue::NULL_KIND:
            attach = makeNull();
            break;

            case JsonlangJsonValue::ARRAY: {
                attach = makeArray(std::vector<HeapThunk*>{});
                auto *arr = static_cast<HeapArray*>(attach.v.h);
                for (size_t i = 0; i < v->elements.size() ; ++i) {
                    arr->elements.push_back(
                        makeHeap<HeapThunk>(idArrayElement, nullptr, 0, nullptr));
                    arr->elements[i]->filled = true;
                    jsonToHeap(v->elements[i], arr->elements[i]->content);
                }
            } break;

            case JsonlangJsonValue::OBJECT: {
                attach = makeObject<HeapComprehensionObject>(
                    BindingFrame{}, jsonObjVar, idJsonObjVar, BindingFrame{});
                auto *obj = static_cast<HeapComprehensionObject*>(attach.v.h);
                for (const auto &pair : v->fields) {
                    auto *thunk = makeHeap<HeapThunk>(idJsonObjVar, nullptr, 0, nullptr);
                    obj->compValues[alloc->makeIdentifier(decode_utf8(pair.first))] = thunk;
                    thunk->filled = true;
                    jsonToHeap(pair.second, thunk->content);
                }
            } break;
        }
    }


    String toString(const LocationRange &loc)
    {
        return manifestJson(loc, false, U"");
    }



    /** Recursively collect an object's invariants.
     *
     * \param curr
     * \param self
     * \param offset
     * \param thunks
     */
    void objectInvariants(HeapObject *curr, HeapObject *self,
                          unsigned &counter, std::vector<HeapThunk*> &thunks)
    {
        if (auto *ext = dynamic_cast<HeapExtendedObject*>(curr)) {
            objectInvariants(ext->right, self, counter, thunks);
            objectInvariants(ext->left, self, counter, thunks);
        } else {
            if (auto *simp = dynamic_cast<HeapSimpleObject*>(curr)) {
                for (AST *assert : simp->asserts) {
                    auto *el_th = makeHeap<HeapThunk>(idInvariant, self, counter, assert);
                    el_th->upValues = simp->upValues;
                    thunks.push_back(el_th);
                }
            }
            counter++;
        }
    }

    /** Index an object's field.
     *
     * \param loc Location where the e.f occured.
     * \param obj The target
     * \param f The field
     */
    const AST *objectIndex(const LocationRange &loc, HeapObject *obj,
                           const Identifier *f, unsigned offset)
    {
        unsigned found_at = 0;
        HeapObject *self = obj;
        HeapLeafObject *found = findObject(f, obj, offset, found_at);
        if (found == nullptr) {
            throw makeError(loc, "Field does not exist: " + encode_utf8(f->name));
        }
        if (auto *simp = dynamic_cast<HeapSimpleObject*>(found)) {
            auto it = simp->fields.find(f);
            const AST *body = it->second.body;

            stack.newCall(loc, simp, self, found_at, simp->upValues);
            return body;
        } else {
            // If a HeapLeafObject is not HeapSimpleObject, it must be HeapComprehensionObject.
            auto *comp = static_cast<HeapComprehensionObject*>(found);
            auto it = comp->compValues.find(f);
            auto *th = it->second;
            BindingFrame binds = comp->upValues;
            binds[comp->id] = th;
            stack.newCall(loc, comp, self, found_at, binds);
            return comp->value;
        }
    }

    void runInvariants(const LocationRange &loc, HeapObject *self)
    {
        if (stack.alreadyExecutingInvariants(self)) return;

        unsigned counter = 0;
        unsigned initial_stack_size = stack.size();
        stack.newFrame(FRAME_INVARIANTS, loc);
        std::vector<HeapThunk*> &thunks = stack.top().thunks;
        objectInvariants(self, self, counter, thunks);
        if (thunks.size() == 0) {
            stack.pop();
            return;
        }
        HeapThunk *thunk = thunks[0];
        stack.top().elementId = 1;
        stack.top().self = self;
        stack.newCall(loc, thunk, thunk->self, thunk->offset, thunk->upValues);
        evaluate(thunk->body, initial_stack_size);
    }

    /** Evaluate the given AST to a value.
     *
     * Rather than call itself recursively, this function maintains a separate stack of
     * partially-evaluated constructs.  First, the AST is handled depending on its type.  If
     * this cannot be completed without evaluating another AST (e.g. a sub expression) then a
     * frame is pushed onto the stack containing the partial state, and the code jumps back to
     * the beginning of this function.  Once there are no more ASTs to evaluate, the code
     * executes the second part of the function to unwind the stack.  If the stack cannot be
     * completely unwound without evaluating an AST then it jumps back to the beginning of the
     * function again.  The process terminates when the AST has been processed and the stack is
     * the same size it was at the beginning of the call to evaluate.
     */
    void evaluate(const AST *ast_, unsigned initial_stack_size)
    {
        recurse:

        switch (ast_->type) {
            case AST_APPLY: {
                const auto &ast = *static_cast<const Apply*>(ast_);
                stack.newFrame(FRAME_APPLY_TARGET, ast_);
                ast_ = ast.target;
                goto recurse;
            } break;

            case AST_ARRAY: {
                const auto &ast = *static_cast<const Array*>(ast_);
                HeapObject *self;
                unsigned offset;
                stack.getSelfBinding(self, offset);
                scratch = makeArray({});
                auto &elements = static_cast<HeapArray*>(scratch.v.h)->elements;
                for (const auto &el : ast.elements) {
                    auto *el_th = makeHeap<HeapThunk>(idArrayElement, self, offset, el.expr);
                    el_th->upValues =  capture(el.expr->freeVariables);
                    elements.push_back(el_th);
                }
            } break;

            case AST_BINARY: {
                const auto &ast = *static_cast<const Binary*>(ast_);
                stack.newFrame(FRAME_BINARY_LEFT, ast_);
                ast_ = ast.left;
                goto recurse;
            } break;

            case AST_BUILTIN_FUNCTION: {
                const auto &ast = *static_cast<const BuiltinFunction*>(ast_);
                HeapClosure::Params params;
                params.reserve(ast.params.size());
                for (const auto &p : ast.params) {
                    // None of the builtins have default args.
                    params.emplace_back(p, nullptr);
                }
                scratch = makeBuiltin(ast.name, params);
            } break;

            case AST_CONDITIONAL: {
                const auto &ast = *static_cast<const Conditional*>(ast_);
                stack.newFrame(FRAME_IF, ast_);
                ast_ = ast.cond;
                goto recurse;
            } break;

            case AST_ERROR: {
                const auto &ast = *static_cast<const Error*>(ast_);
                stack.newFrame(FRAME_ERROR, ast_);
                ast_ = ast.expr;
                goto recurse;
            } break;

            // lambda - b
            case AST_EXEC: {
                const auto &ast = *static_cast<const Exec*>(ast_);
                AST *expr = exec(ast.location, ast.file);
                ast_ = expr;
                stack.newCall(ast.location, nullptr, nullptr, 0, BindingFrame());
                goto recurse;
            } break;
            // lambda - e

            case AST_FUNCTION: {
                const auto &ast = *static_cast<const Function*>(ast_);
                auto env = capture(ast.freeVariables);
                HeapObject *self;
                unsigned offset;
                stack.getSelfBinding(self, offset);
                HeapClosure::Params params;
                params.reserve(ast.params.size());
                for (const auto &p : ast.params) {
                    params.emplace_back(p.id, p.expr);
                }
                scratch = makeClosure(env, self, offset, params, ast.body);
            } break;

            case AST_IMPORT: {
                const auto &ast = *static_cast<const Import*>(ast_);
                AST *expr = import(ast.location, ast.file);
                ast_ = expr;
                stack.newCall(ast.location, nullptr, nullptr, 0, BindingFrame());
                goto recurse;
            } break;

            case AST_IMPORTSTR: {
                const auto &ast = *static_cast<const Importstr*>(ast_);
                const ImportCacheValue *value = importString(ast.location, ast.file);
                scratch = makeString(decode_utf8(value->content));
            } break;

            case AST_INDEX: {
                const auto &ast = *static_cast<const Index*>(ast_);
                stack.newFrame(FRAME_INDEX_TARGET, ast_);
                ast_ = ast.target;
                goto recurse;
            } break;

            case AST_LOCAL: {
                const auto &ast = *static_cast<const Local*>(ast_);
                stack.newFrame(FRAME_LOCAL, ast_);
                Frame &f = stack.top();
                // First build all the thunks and bind them.
                HeapObject *self;
                unsigned offset;
                stack.getSelfBinding(self, offset);
                for (const auto &bind : ast.binds) {
                    // Note that these 2 lines must remain separate to avoid the GC running
                    // when bindings has a nullptr for key bind.first.
                    auto *th = makeHeap<HeapThunk>(bind.var, self, offset, bind.body);
                    f.bindings[bind.var] = th;
                }
                // Now capture the environment (including the new thunks, to make cycles).
                for (const auto &bind : ast.binds) {
                    auto *thunk = f.bindings[bind.var];
                    thunk->upValues = capture(bind.body->freeVariables);
                }
                ast_ = ast.body;
                goto recurse;
            } break;

            case AST_LITERAL_BOOLEAN: {
                const auto &ast = *static_cast<const LiteralBoolean*>(ast_);
                scratch = makeBoolean(ast.value);
            } break;

            case AST_LITERAL_NUMBER: {
                const auto &ast = *static_cast<const LiteralNumber*>(ast_);
                scratch = makeDoubleCheck(ast_->location, ast.value);
            } break;

            case AST_LITERAL_STRING: {
                const auto &ast = *static_cast<const LiteralString*>(ast_);
                scratch = makeString(ast.value);
            } break;

            case AST_LITERAL_NULL: {
                scratch = makeNull();
            } break;

            case AST_DESUGARED_OBJECT: {
                const auto &ast = *static_cast<const DesugaredObject*>(ast_);
                if (ast.fields.empty()) {
                    auto env = capture(ast.freeVariables);
                    std::map<const Identifier *, HeapSimpleObject::Field> fields;
                    scratch = makeObject<HeapSimpleObject>(env, fields, ast.asserts);
                } else {
                    auto env = capture(ast.freeVariables);
                    stack.newFrame(FRAME_OBJECT, ast_);
                    auto fit = ast.fields.begin();
                    stack.top().fit = fit;
                    ast_ = fit->name;
                    goto recurse;
                }
            } break;

            case AST_OBJECT_COMPREHENSION_SIMPLE: {
                const auto &ast = *static_cast<const ObjectComprehensionSimple*>(ast_);
                stack.newFrame(FRAME_OBJECT_COMP_ARRAY, ast_);
                ast_ = ast.array;
                goto recurse;
            } break;

            case AST_SELF: {
                scratch.t = Value::OBJECT;
                HeapObject *self;
                unsigned offset;
                stack.getSelfBinding(self, offset);
                scratch.v.h = self;
            } break;

            case AST_SUPER_INDEX: {
                const auto &ast = *static_cast<const SuperIndex*>(ast_);
                stack.newFrame(FRAME_SUPER_INDEX, ast_);
                ast_ = ast.index;
                goto recurse;
            } break;

            case AST_UNARY: {
                const auto &ast = *static_cast<const Unary*>(ast_);
                stack.newFrame(FRAME_UNARY, ast_);
                ast_ = ast.expr;
                goto recurse;
            } break;

            case AST_VAR: {
                const auto &ast = *static_cast<const Var*>(ast_);
                auto *thunk = stack.lookUpVar(ast.id);
                if (thunk == nullptr) {
                    std::cerr << "INTERNAL ERROR: Could not bind variable: "
                              << encode_utf8(ast.id->name) << std::endl;
                    std::abort();
                }
                if (thunk->filled) {
                    scratch = thunk->content;
                } else {
                    stack.newCall(ast.location, thunk, thunk->self, thunk->offset, thunk->upValues);
                    ast_ = thunk->body;
                    goto recurse;
                }
            } break;

            default:
            std::cerr << "INTERNAL ERROR: Unknown AST: " << ast_->type << std::endl;
            std::abort();
        }

        // To evaluate another AST, set ast to it, then goto recurse.
        // To pop, exit the switch or goto popframe
        // To change the frame and re-enter the switch, goto replaceframe
        while (stack.size() > initial_stack_size) {
            Frame &f = stack.top();
            switch (f.kind) {
                case FRAME_APPLY_TARGET: {
                    const auto &ast = *static_cast<const Apply*>(f.ast);
                    if (scratch.t != Value::FUNCTION) {
                        throw makeError(ast.location,
                                        "Only functions can be called, got "
                                        + type_str(scratch));
                    }
                    auto *func = static_cast<HeapClosure*>(scratch.v.h);

                    std::set<const Identifier *> params_needed;
                    for (const auto &param : func->params) {
                        params_needed.insert(param.id);
                    }

                    // Create thunks for arguments.
                    std::vector<HeapThunk*> positional_args;
                    BindingFrame args;
                    bool got_named = false;
                    for (unsigned i=0 ; i<ast.args.size() ; ++i) {
                        const auto &arg = ast.args[i];

                        const Identifier *name;
                        if (arg.id != nullptr) {
                            got_named = true;
                            name = arg.id;
                        } else {
                            if (got_named) {
                                std::stringstream ss;
                                ss << "Internal error: got positional param after named at index "
                                   << i;
                                throw makeError(ast.location, ss.str());
                            }
                            if (i >= func->params.size()) {
                                std::stringstream ss;
                                ss << "Too many args, function has " << func->params.size()
                                   << " parameter(s)";
                                throw makeError(ast.location, ss.str());
                            }
                            name = func->params[i].id;
                        }
                        // Special case for builtin functions -- leave identifier blank for
                        // them in the thunk.  This removes the thunk frame from the stacktrace.
                        const Identifier *name_ = func->body == nullptr ? nullptr : name;
                        HeapObject *self;
                        unsigned offset;
                        stack.getSelfBinding(self, offset);
                        auto *thunk = makeHeap<HeapThunk>(name_, self, offset, arg.expr);
                        thunk->upValues = capture(arg.expr->freeVariables);
                        // While making the thunks, keep them in a frame to avoid premature garbage
                        // collection.
                        f.thunks.push_back(thunk);
                        if (args.find(name) != args.end()) {
                            std::stringstream ss;
                            ss << "Binding parameter a second time: " << encode_utf8(name->name);
                            throw makeError(ast.location, ss.str());
                        }
                        args[name] = thunk;
                        if (params_needed.find(name) == params_needed.end()) {
                            std::stringstream ss;
                            ss << "Function has no parameter " << encode_utf8(name->name);
                            throw makeError(ast.location, ss.str());
                        }
                    }

                    // For any func params for which there was no arg, create a thunk for those and
                    // bind the default argument.  Allow default thunks to see other params.  If no
                    // default argument than raise an error.

                    // Raise errors for unbound params, create thunks (but don't fill in upvalues).
                    // This is a subset of f.thunks, so will not get garbage collected.
                    std::vector<HeapThunk*> def_arg_thunks;
                    for (const auto &param : func->params) {
                        if (args.find(param.id) != args.end()) continue;
                        if (param.def == nullptr) {
                            std::stringstream ss;
                            ss << "Function parameter " << encode_utf8(param.id->name) <<
                                  " not bound in call.";
                            throw makeError(ast.location, ss.str());
                        }

                        // Special case for builtin functions -- leave identifier blank for
                        // them in the thunk.  This removes the thunk frame from the stacktrace.
                        const Identifier *name_ = func->body == nullptr ? nullptr : param.id;
                        auto *thunk = makeHeap<HeapThunk>(name_, func->self, func->offset,
                                                          param.def);
                        f.thunks.push_back(thunk);
                        def_arg_thunks.push_back(thunk);
                        args[param.id] = thunk;
                    }

                    BindingFrame up_values = func->upValues;
                    up_values.insert(args.begin(), args.end());

                    // Fill in upvalues
                    for (HeapThunk *thunk : def_arg_thunks) {
                        thunk->upValues = up_values;
                    }

                    // Cache these, because pop will invalidate them.
                    std::vector<HeapThunk*> thunks_copy = f.thunks;

                    stack.pop();

                    if (func->body == nullptr) {
                        // Built-in function.
                        // Give nullptr for self because noone looking at this frame will
                        // attempt to bind to self (it's native code).
                        stack.newFrame(FRAME_BUILTIN_FORCE_THUNKS, f.ast);
                        stack.top().thunks = thunks_copy;
                        stack.top().val = scratch;
                        goto replaceframe;
                    } else {
                        // User defined function.
                        stack.newCall(ast.location, func, func->self, func->offset, up_values);
                        if (ast.tailstrict) {
                            stack.top().tailCall = true;
                            if (thunks_copy.size() == 0) {
                                // No need to force thunks, proceed straight to body.
                                ast_ = func->body;
                                goto recurse;
                            } else {
                                // The check for args.size() > 0
                                stack.top().thunks = thunks_copy;
                                stack.top().val = scratch;
                                goto replaceframe;
                            }
                        } else {
                            ast_ = func->body;
                            goto recurse;
                        }
                    }
                } break;

                case FRAME_BINARY_LEFT: {
                    const auto &ast = *static_cast<const Binary*>(f.ast);
                    const Value &lhs = scratch;
                    if (lhs.t == Value::BOOLEAN) {
                        // Handle short-cut semantics
                        switch (ast.op) {
                            case BOP_AND: {
                                if (!lhs.v.b) {
                                    scratch = makeBoolean(false);
                                    goto popframe;
                                }
                            } break;

                            case BOP_OR: {
                                if (lhs.v.b) {
                                    scratch = makeBoolean(true);
                                    goto popframe;
                                }
                            } break;

                            default:;
                        }
                    }
                    stack.top().kind = FRAME_BINARY_RIGHT;
                    stack.top().val = lhs;
                    ast_ = ast.right;
                    goto recurse;
                } break;

                case FRAME_BINARY_RIGHT: {
                    const auto &ast = *static_cast<const Binary*>(f.ast);
                    const Value &lhs = stack.top().val;
                    const Value &rhs = scratch;
                    if (lhs.t == Value::STRING || rhs.t == Value::STRING) {
                        if (ast.op == BOP_PLUS) {
                            // Handle co-ercions for string processing.
                            stack.top().kind = FRAME_STRING_CONCAT;
                            stack.top().val2 = rhs;
                            goto replaceframe;
                        }
                    }
                    // Equality can be used when the types don't match.
                    switch (ast.op) {
                        case BOP_MANIFEST_EQUAL:
                        std::cerr << "INTERNAL ERROR: Equals not desugared" << std::endl;
                        abort();

                        case BOP_MANIFEST_UNEQUAL:
                        std::cerr << "INTERNAL ERROR: Notequals not desugared" << std::endl;
                        abort();

                        default:;
                    }
                    // Everything else requires matching types.
                    if (lhs.t != rhs.t) {
                        throw makeError(ast.location,
                                        "Binary operator " + bop_string(ast.op) + " requires "
                                        "matching types, got " + type_str(lhs) + " and " +
                                        type_str(rhs) + ".");
                    }
                    switch (lhs.t) {
                        case Value::ARRAY:
                        if (ast.op == BOP_PLUS) {
                            auto *arr_l = static_cast<HeapArray*>(lhs.v.h);
                            auto *arr_r = static_cast<HeapArray*>(rhs.v.h);
                            std::vector<HeapThunk*> elements;
                            for (auto *el : arr_l->elements)
                                elements.push_back(el);
                            for (auto *el : arr_r->elements)
                                elements.push_back(el);
                            scratch = makeArray(elements);
                        } else {
                            throw makeError(ast.location,
                                            "Binary operator " + bop_string(ast.op)
                                            + " does not operate on arrays.");
                        }
                        break;

                        case Value::BOOLEAN:
                        switch (ast.op) {
                            case BOP_AND:
                            scratch = makeBoolean(lhs.v.b && rhs.v.b);
                            break;

                            case BOP_OR:
                            scratch = makeBoolean(lhs.v.b || rhs.v.b);
                            break;

                            default:
                            throw makeError(ast.location,
                                            "Binary operator " + bop_string(ast.op)
                                            + " does not operate on booleans.");
                        }
                        break;

                        case Value::DOUBLE:
                        switch (ast.op) {
                            case BOP_PLUS:
                            scratch = makeDoubleCheck(ast.location, lhs.v.d + rhs.v.d);
                            break;

                            case BOP_MINUS:
                            scratch = makeDoubleCheck(ast.location, lhs.v.d - rhs.v.d);
                            break;

                            case BOP_MULT:
                            scratch = makeDoubleCheck(ast.location, lhs.v.d * rhs.v.d);
                            break;

                            case BOP_DIV:
                            if (rhs.v.d == 0)
                                throw makeError(ast.location, "Division by zero.");
                            scratch = makeDoubleCheck(ast.location, lhs.v.d / rhs.v.d);
                            break;

                            // No need to check doubles made from longs

                            case BOP_SHIFT_L: {
                                long long_l = lhs.v.d;
                                long long_r = rhs.v.d;
                                scratch = makeDouble(long_l << long_r);
                            } break;

                            case BOP_SHIFT_R: {
                                long long_l = lhs.v.d;
                                long long_r = rhs.v.d;
                                scratch = makeDouble(long_l >> long_r);
                            } break;

                            case BOP_BITWISE_AND: {
                                long long_l = lhs.v.d;
                                long long_r = rhs.v.d;
                                scratch = makeDouble(long_l & long_r);
                            } break;

                            case BOP_BITWISE_XOR: {
                                long long_l = lhs.v.d;
                                long long_r = rhs.v.d;
                                scratch = makeDouble(long_l ^ long_r);
                            } break;

                            case BOP_BITWISE_OR: {
                                long long_l = lhs.v.d;
                                long long_r = rhs.v.d;
                                scratch = makeDouble(long_l | long_r);
                            } break;

                            case BOP_LESS_EQ:
                            scratch = makeBoolean(lhs.v.d <= rhs.v.d);
                            break;

                            case BOP_GREATER_EQ:
                            scratch = makeBoolean(lhs.v.d >= rhs.v.d);
                            break;

                            case BOP_LESS:
                            scratch = makeBoolean(lhs.v.d < rhs.v.d);
                            break;

                            case BOP_GREATER:
                            scratch = makeBoolean(lhs.v.d > rhs.v.d);
                            break;

                            default:
                            throw makeError(ast.location,
                                            "Binary operator " + bop_string(ast.op)
                                            + " does not operate on numbers.");
                        }
                        break;

                        case Value::FUNCTION:
                        throw makeError(ast.location, "Binary operator " + bop_string(ast.op) +
                                                      " does not operate on functions.");

                        case Value::NULL_TYPE:
                        throw makeError(ast.location, "Binary operator " + bop_string(ast.op) +
                                                      " does not operate on null.");

                        case Value::OBJECT: {
                            if (ast.op != BOP_PLUS) {
                                throw makeError(ast.location,
                                                "Binary operator " + bop_string(ast.op) +
                                                " does not operate on objects.");
                            }
                            auto *lhs_obj = static_cast<HeapObject*>(lhs.v.h);
                            auto *rhs_obj = static_cast<HeapObject*>(rhs.v.h);
                            scratch = makeObject<HeapExtendedObject>(lhs_obj, rhs_obj);
                        }
                        break;

                        case Value::STRING: {
                            const String &lhs_str =
                                static_cast<HeapString*>(lhs.v.h)->value;
                            const String &rhs_str =
                                static_cast<HeapString*>(rhs.v.h)->value;
                            switch (ast.op) {
                                case BOP_PLUS:
                                scratch = makeString(lhs_str + rhs_str);
                                break;

                                case BOP_LESS_EQ:
                                scratch = makeBoolean(lhs_str <= rhs_str);
                                break;

                                case BOP_GREATER_EQ:
                                scratch = makeBoolean(lhs_str >= rhs_str);
                                break;

                                case BOP_LESS:
                                scratch = makeBoolean(lhs_str < rhs_str);
                                break;

                                case BOP_GREATER:
                                scratch = makeBoolean(lhs_str > rhs_str);
                                break;

                                default:
                                throw makeError(ast.location,
                                                "Binary operator " + bop_string(ast.op)
                                                + " does not operate on strings.");
                            }
                        }
                        break;
                    }
                } break;

                case FRAME_BUILTIN_FILTER: {
                    const auto &ast = *static_cast<const Apply*>(f.ast);
                    auto *func = static_cast<HeapClosure*>(f.val.v.h);
                    auto *arr = static_cast<HeapArray*>(f.val2.v.h);
                    if (scratch.t != Value::BOOLEAN) {
                        throw makeError(ast.location,
                                        "filter function must return boolean, got: "
                                        + type_str(scratch));
                    }
                    if (scratch.v.b) f.thunks.push_back(arr->elements[f.elementId]);
                    f.elementId++;
                    // Iterate through arr, calling the function on each.
                    if (f.elementId == arr->elements.size()) {
                        scratch = makeArray(f.thunks);
                    } else {
                        auto *thunk = arr->elements[f.elementId];
                        BindingFrame bindings = func->upValues;
                        bindings[func->params[0].id] = thunk;
                        stack.newCall(ast.location, func, func->self, func->offset, bindings);
                        ast_ = func->body;
                        goto recurse;
                    }
                } break;

                case FRAME_BUILTIN_FORCE_THUNKS: {
                    const auto &ast = *static_cast<const Apply*>(f.ast);
                    auto *func = static_cast<HeapClosure*>(f.val.v.h);
                    if (f.elementId == f.thunks.size()) {
                        // All thunks forced, now the builtin implementations.
                        const LocationRange &loc = ast.location;
                        const std::string &builtin_name = func->builtinName;
                        std::vector<Value> args;
                        for (auto *th : f.thunks) {
                            args.push_back(th->content);
                        }
                        BuiltinMap::const_iterator bit = builtins.find(builtin_name);
                        if (bit != builtins.end()) {
                            const AST *new_ast = (this->*bit->second)(loc, args);
                            if (new_ast != nullptr) {
                                ast_ = new_ast;
                                goto recurse;
                            }
                            break;
                        }
                        VmNativeCallbackMap::const_iterator nit =
                            nativeCallbacks.find(builtin_name);
                        // TODO(dcunnin): Support arrays.
                        // TODO(dcunnin): Support objects.
                        std::vector<JsonlangJsonValue> args2;
                        for (const Value &arg : args) {
                            switch (arg.t) {
                                case Value::STRING:
                                args2.push_back(JsonlangJsonValue{
                                    JsonlangJsonValue::STRING,
                                    encode_utf8(static_cast<HeapString*>(arg.v.h)->value),
                                    0,
                                    std::vector<std::unique_ptr<JsonlangJsonValue>>{},
                                    std::map<std::string, std::unique_ptr<JsonlangJsonValue>>{},
                                });
                                break;

                                case Value::BOOLEAN:
                                args2.push_back(JsonlangJsonValue{
                                    JsonlangJsonValue::BOOL,
                                    "",
                                    arg.v.b ? 1.0 : 0.0,
                                    std::vector<std::unique_ptr<JsonlangJsonValue>>{},
                                    std::map<std::string, std::unique_ptr<JsonlangJsonValue>>{},
                                });
                                break;

                                case Value::DOUBLE:
                                args2.push_back(JsonlangJsonValue{
                                    JsonlangJsonValue::NUMBER,
                                    "",
                                    arg.v.d,
                                    std::vector<std::unique_ptr<JsonlangJsonValue>>{},
                                    std::map<std::string, std::unique_ptr<JsonlangJsonValue>>{},
                                });
                                break;

                                case Value::NULL_TYPE:
                                args2.push_back(JsonlangJsonValue{
                                    JsonlangJsonValue::NULL_KIND,
                                    "",
                                    0,
                                    std::vector<std::unique_ptr<JsonlangJsonValue>>{},
                                    std::map<std::string, std::unique_ptr<JsonlangJsonValue>>{},
                                });
                                break;

                                default:
                                throw makeError(ast.location,
                                                "Native extensions can only take primitives.");
                            }

                        }
                        std::vector<const JsonlangJsonValue*> args3;
                        for (size_t i = 0; i < args2.size() ; ++i) {
                            args3.push_back(&args2[i]);
                        }
                        if (nit == nativeCallbacks.end()) {
                            throw makeError(ast.location,
                                            "Unrecognized builtin name: " + builtin_name);
                        }
                        const VmNativeCallback &cb = nit->second;

                        int succ;
                        std::unique_ptr<JsonlangJsonValue> r(cb.cb(cb.ctx, &args3[0], &succ));

                        if (succ) {
                            jsonToHeap(r, scratch);
                        } else {
                            if (r->kind != JsonlangJsonValue::STRING) {
                                throw makeError(
                                    ast.location,
                                    "Native extension returned an error that was not a string.");
                            }
                            std::string rs = r->string;
                            throw makeError(ast.location, rs);
                        }

                    } else {
                        // Not all arguments forced yet.
                        HeapThunk *th = f.thunks[f.elementId++];
                        if (!th->filled) {
                            stack.newCall(ast.location, th, th->self, th->offset, th->upValues);
                            ast_ = th->body;
                            goto recurse;
                        }
                    }
                } break;

                case FRAME_CALL: {
                    if (auto *thunk = dynamic_cast<HeapThunk*>(f.context)) {
                        // If we called a thunk, cache result.
                        thunk->fill(scratch);
                    } else if (auto *closure = dynamic_cast<HeapClosure*>(f.context)) {
                        if (f.elementId < f.thunks.size()) {
                            // If tailstrict, force thunks
                            HeapThunk *th = f.thunks[f.elementId++];
                            if (!th->filled) {
                                stack.newCall(f.location, th,
                                              th->self, th->offset, th->upValues);
                                ast_ = th->body;
                                goto recurse;
                            }
                        } else if (f.thunks.size() == 0) {
                            // Body has now been executed
                        } else {
                            // Execute the body
                            f.thunks.clear();
                            f.elementId = 0;
                            ast_ = closure->body;
                            goto recurse;
                        }
                    }
                    // Result of call is in scratch, just pop.
                } break;

                case FRAME_ERROR: {
                    const auto &ast = *static_cast<const Error*>(f.ast);
                    if (scratch.t != Value::STRING)
                        throw makeError(ast.location, "Error message must be string, got " +
                                                      type_str(scratch) + ".");
                    std::string msg = encode_utf8(static_cast<HeapString*>(scratch.v.h)->value);
                    throw makeError(ast.location, msg);
                } break;

                case FRAME_IF: {
                    const auto &ast = *static_cast<const Conditional*>(f.ast);
                    if (scratch.t != Value::BOOLEAN) {
                        throw makeError(ast.location, "Condition must be boolean, got " +
                                                      type_str(scratch) + ".");
                    }
                    ast_ = scratch.v.b ? ast.branchTrue : ast.branchFalse;
                    stack.pop();
                    goto recurse;
                } break;

                case FRAME_SUPER_INDEX: {
                    const auto &ast = *static_cast<const SuperIndex*>(f.ast);
                    HeapObject *self;
                    unsigned offset;
                    stack.getSelfBinding(self, offset);
                    offset++;
                    if (offset >= countLeaves(self)) {
                        throw makeError(ast.location,
                                        "Attempt to use super when there is no super class.");
                    }
                    if (scratch.t != Value::STRING) {
                        throw makeError(ast.location,
                                        "Super index must be string, got "
                                        + type_str(scratch) + ".");
                    }

                    const String &index_name =
                        static_cast<HeapString*>(scratch.v.h)->value;
                    auto *fid = alloc->makeIdentifier(index_name);
                    stack.pop();
                    ast_ = objectIndex(ast.location, self, fid, offset);
                    goto recurse;
                } break;

                case FRAME_INDEX_INDEX: {
                    const auto &ast = *static_cast<const Index*>(f.ast);
                    const Value &target = f.val;
                    if (target.t == Value::ARRAY) {
                        const auto *array = static_cast<HeapArray*>(target.v.h);
                        if (scratch.t != Value::DOUBLE) {
                            throw makeError(ast.location, "Array index must be number, got "
                                                          + type_str(scratch) + ".");
                        }
                        long i = long(scratch.v.d);
                        long sz = array->elements.size();
                        if (i < 0 || i >= sz) {
                            std::stringstream ss;
                            ss << "Array bounds error: " << i
                               << " not within [0, " << sz << ")";
                            throw makeError(ast.location, ss.str());
                        }
                        auto *thunk = array->elements[i];
                        if (thunk->filled) {
                            scratch = thunk->content;
                        } else {
                            stack.pop();
                            stack.newCall(ast.location, thunk,
                                          thunk->self, thunk->offset, thunk->upValues);
                            ast_ = thunk->body;
                            goto recurse;
                        }
                    } else if (target.t == Value::OBJECT) {
                        auto *obj = static_cast<HeapObject*>(target.v.h);
                        assert(obj != nullptr);
                        if (scratch.t != Value::STRING) {
                            throw makeError(ast.location,
                                            "Object index must be string, got "
                                            + type_str(scratch) + ".");
                        }
                        const String &index_name =
                            static_cast<HeapString*>(scratch.v.h)->value;
                        auto *fid = alloc->makeIdentifier(index_name);
                        stack.pop();
                        ast_ = objectIndex(ast.location, obj, fid, 0);
                        goto recurse;
                    } else if (target.t == Value::STRING) {
                        auto *obj = static_cast<HeapString*>(target.v.h);
                        assert(obj != nullptr);
                        if (scratch.t != Value::DOUBLE) {
                            throw makeError(ast.location,
                                            "String index must be a number, got "
                                            + type_str(scratch) + ".");
                        }
                        long sz = obj->value.length();
                        long i = (long)scratch.v.d;
                        if (i < 0 || i >= sz) {
                            std::stringstream ss;
                            ss << "String bounds error: " << i
                               << " not within [0, " << sz << ")";
                            throw makeError(ast.location, ss.str());
                        }
                        char32_t ch[] = {obj->value[i], U'\0'};
                        scratch = makeString(ch);
                    } else {
                        std::cerr << "INTERNAL ERROR: Not object / array / string." << std::endl;
                        abort();
                    }
                } break;

                case FRAME_INDEX_TARGET: {
                    const auto &ast = *static_cast<const Index*>(f.ast);
                    if (scratch.t != Value::ARRAY
                        && scratch.t != Value::OBJECT
                        && scratch.t != Value::STRING) {
                        throw makeError(ast.location,
                                        "Can only index objects, strings, and arrays, got "
                                        + type_str(scratch) + ".");
                    }
                    f.val = scratch;
                    f.kind = FRAME_INDEX_INDEX;
                    if (scratch.t == Value::OBJECT) {
                        auto *self = static_cast<HeapObject*>(scratch.v.h);
                        if (!stack.alreadyExecutingInvariants(self)) {
                            stack.newFrame(FRAME_INVARIANTS, ast.location);
                            Frame &f2 = stack.top();
                            f2.self = self;
                            unsigned counter = 0;
                            objectInvariants(self, self, counter, f2.thunks);
                            if (f2.thunks.size() > 0) {
                                auto *thunk = f2.thunks[0];
                                f2.elementId = 1;
                                stack.newCall(ast.location, thunk,
                                              thunk->self, thunk->offset, thunk->upValues);
                                ast_ = thunk->body;
                                goto recurse;
                            }
                        }
                    }
                    ast_ = ast.index;
                    goto recurse;
                } break;

                case FRAME_INVARIANTS: {
                    if (f.elementId >= f.thunks.size()) {
                        if (stack.size() == initial_stack_size + 1) {
                            // Just pop, evaluate was invoked by runInvariants.
                            break;
                        }
                        stack.pop();
                        Frame &f2 = stack.top();
                        const auto &ast = *static_cast<const Index*>(f2.ast);
                        ast_ = ast.index;
                        goto recurse;
                    }
                    auto *thunk = f.thunks[f.elementId++];
                    stack.newCall(f.location, thunk,
                                  thunk->self, thunk->offset, thunk->upValues);
                    ast_ = thunk->body;
                    goto recurse;
                } break;

                case FRAME_LOCAL: {
                    // Result of execution is in scratch already.
                } break;

                case FRAME_OBJECT: {
                    const auto &ast = *static_cast<const DesugaredObject*>(f.ast);
                    if (scratch.t != Value::NULL_TYPE) {
                        if (scratch.t != Value::STRING) {
                            throw makeError(ast.location, "Field name was not a string.");
                        }
                        const auto &fname = static_cast<const HeapString*>(scratch.v.h)->value;
                        const Identifier *fid = alloc->makeIdentifier(fname);
                        if (f.objectFields.find(fid) != f.objectFields.end()) {
                            std::string msg = "Duplicate field name: \""
                                              + encode_utf8(fname) + "\"";
                            throw makeError(ast.location, msg);
                        }
                        f.objectFields[fid].hide = f.fit->hide;
                        f.objectFields[fid].body = f.fit->body;
                    }
                    f.fit++;
                    if (f.fit != ast.fields.end()) {
                        ast_ = f.fit->name;
                        goto recurse;
                    } else {
                        auto env = capture(ast.freeVariables);
                        scratch = makeObject<HeapSimpleObject>(env, f.objectFields, ast.asserts);
                    }
                } break;

                case FRAME_OBJECT_COMP_ARRAY: {
                    const auto &ast = *static_cast<const ObjectComprehensionSimple*>(f.ast);
                    const Value &arr_v = scratch;
                    if (scratch.t != Value::ARRAY) {
                        throw makeError(ast.location,
                                        "Object comprehension needs array, got "
                                        + type_str(arr_v));
                    }
                    const auto *arr = static_cast<const HeapArray*>(arr_v.v.h);
                    if (arr->elements.size() == 0) {
                        // Degenerate case.  Just create the object now.
                        scratch = makeObject<HeapComprehensionObject>(BindingFrame{}, ast.value,
                                                                      ast.id, BindingFrame{});
                    } else {
                        f.kind = FRAME_OBJECT_COMP_ELEMENT;
                        f.val = scratch;
                        f.bindings[ast.id] = arr->elements[0];
                        f.elementId = 0;
                        ast_ = ast.field;
                        goto recurse;
                    }
                } break;

                case FRAME_OBJECT_COMP_ELEMENT: {
                    const auto &ast = *static_cast<const ObjectComprehensionSimple*>(f.ast);
                    const auto *arr = static_cast<const HeapArray*>(f.val.v.h);
                    if (scratch.t != Value::STRING) {
                        std::stringstream ss;
                        ss << "field must be string, got: " << type_str(scratch);
                        throw makeError(ast.location, ss.str());
                    }
                    const auto &fname = static_cast<const HeapString*>(scratch.v.h)->value;
                    const Identifier *fid = alloc->makeIdentifier(fname);
                    if (f.elements.find(fid) != f.elements.end()) {
                        throw makeError(ast.location,
                                        "Duplicate field name: \"" + encode_utf8(fname) + "\"");
                    }
                    f.elements[fid] = arr->elements[f.elementId];
                    f.elementId++;

                    if (f.elementId == arr->elements.size()) {
                        auto env = capture(ast.freeVariables);
                        scratch = makeObject<HeapComprehensionObject>(env, ast.value,
                                                                      ast.id, f.elements);
                    } else {
                        f.bindings[ast.id] = arr->elements[f.elementId];
                        ast_ = ast.field;
                        goto recurse;
                    }
                } break;

                case FRAME_STRING_CONCAT: {
                    const auto &ast = *static_cast<const Binary*>(f.ast);
                    const Value &lhs = stack.top().val;
                    const Value &rhs = stack.top().val2;
                    String output;
                    if (lhs.t == Value::STRING) {
                        output.append(static_cast<const HeapString*>(lhs.v.h)->value);
                    } else {
                        scratch = lhs;
                        output.append(toString(ast.left->location));
                    }
                    if (rhs.t == Value::STRING) {
                        output.append(static_cast<const HeapString*>(rhs.v.h)->value);
                    } else {
                        scratch = rhs;
                        output.append(toString(ast.right->location));
                    }
                    scratch = makeString(output);
                } break;

                case FRAME_UNARY: {
                    const auto &ast = *static_cast<const Unary*>(f.ast);
                    switch (scratch.t) {

                        case Value::BOOLEAN:
                        if (ast.op == UOP_NOT) {
                            scratch = makeBoolean(!scratch.v.b);
                        } else {
                            throw makeError(ast.location,
                                            "Unary operator " + uop_string(ast.op)
                                            + " does not operate on booleans.");
                        }
                        break;

                        case Value::DOUBLE:
                        switch (ast.op) {
                            case UOP_PLUS:
                            break;

                            case UOP_MINUS:
                            scratch = makeDouble(-scratch.v.d);
                            break;

                            case UOP_BITWISE_NOT:
                            scratch = makeDouble(~(long)(scratch.v.d));
                            break;

                            default:
                            throw makeError(ast.location,
                                            "Unary operator " + uop_string(ast.op)
                                            + " does not operate on numbers.");
                        }
                        break;

                        default:
                            throw makeError(ast.location,
                                            "Unary operator " + uop_string(ast.op) +
                                            " does not operate on type " + type_str(scratch));
                    }
                } break;

                default:
                std::cerr << "INTERNAL ERROR: Unknown FrameKind:  " << f.kind << std::endl;
                std::abort();
            }

            popframe:;

            stack.pop();

            replaceframe:;
        }
    }

    /** Manifest the scratch value by evaluating any remaining fields, and then convert to JSON.
     *
     * This can trigger a garbage collection cycle.  Be sure to stash any objects that aren't
     * reachable via the stack or heap.
     *
     * \param multiline If true, will print objects and arrays in an indented fashion.
     */
    String manifestJson(const LocationRange &loc, bool multiline, const String &indent)
    {
        // Printing fields means evaluating and binding them, which can trigger
        // garbage collection.

        StringStream ss;
        switch (scratch.t) {
            case Value::ARRAY: {
                HeapArray *arr = static_cast<HeapArray*>(scratch.v.h);
                if (arr->elements.size() == 0) {
                    ss << U"[ ]";
                } else {
                    const char32_t *prefix = multiline ? U"[\n" : U"[";
                    String indent2 = multiline ? indent + U"   " : indent;
                    for (auto *thunk : arr->elements) {
                        LocationRange tloc = thunk->body == nullptr
                                           ? loc
                                           : thunk->body->location;
                        if (thunk->filled) {
                            stack.newCall(loc, thunk, nullptr, 0, BindingFrame{});
                            // Keep arr alive when scratch is overwritten
                            stack.top().val = scratch;
                            scratch = thunk->content;
                        } else {
                            stack.newCall(loc, thunk, thunk->self, thunk->offset, thunk->upValues);
                            // Keep arr alive when scratch is overwritten
                            stack.top().val = scratch;
                            evaluate(thunk->body, stack.size());
                        }
                        auto element = manifestJson(tloc, multiline, indent2);
                        // Restore scratch
                        scratch = stack.top().val;
                        stack.pop();
                        ss << prefix << indent2 << element;
                        prefix = multiline ? U",\n" : U", ";
                    }
                    ss << (multiline ? U"\n" : U"") << indent << U"]";
                }
            }
            break;

            case Value::BOOLEAN:
            ss << (scratch.v.b ? U"true" : U"false");
            break;

            case Value::DOUBLE:
            ss << decode_utf8(jsonlang_unparse_number(scratch.v.d));
            break;

            case Value::FUNCTION:
            throw makeError(loc, "Couldn't manifest function in JSON output.");

            case Value::NULL_TYPE:
            ss << U"null";
            break;

            case Value::OBJECT: {
                auto *obj = static_cast<HeapObject*>(scratch.v.h);
                runInvariants(loc, obj);
                // Using std::map has the useful side-effect of ordering the fields
                // alphabetically.
                std::map<String, const Identifier*> fields;
                for (const auto &f : objectFields(obj, true)) {
                    fields[f->name] = f;
                }
                if (fields.size() == 0) {
                    ss << U"{ }";
                } else {
                    String indent2 = multiline ? indent + U"   " : indent;
                    const char32_t *prefix = multiline ? U"{\n" : U"{";
                    for (const auto &f : fields) {
                        // pushes FRAME_CALL
                        const AST *body = objectIndex(loc, obj, f.second, 0);
                        stack.top().val = scratch;
                        evaluate(body, stack.size());
                        auto vstr = manifestJson(body->location, multiline, indent2);
                        // Reset scratch so that the object we're manifesting doesn't
                        // get GC'd.
                        scratch = stack.top().val;
                        stack.pop();
                        ss << prefix << indent2 << U"\"" << f.first << U"\": " << vstr;
                        prefix = multiline ? U",\n" : U", ";
                    }
                    ss << (multiline ? U"\n" : U"") << indent << U"}";
                }
            }
            break;

            case Value::STRING: {
                const String &str = static_cast<HeapString*>(scratch.v.h)->value;
                ss << jsonlang_string_unparse(str, false);
            }
            break;
        }
        return ss.str();
    }

    String manifestString(const LocationRange &loc)
    {
        if (scratch.t != Value::STRING) {
            std::stringstream ss;
            ss << "Expected string result, got: " << type_str(scratch.t);
            throw makeError(loc, ss.str());
        }
        return static_cast<HeapString*>(scratch.v.h)->value;
    }

    StrMap manifestMulti(bool string)
    {
        StrMap r;
        LocationRange loc("During manifestation");
        if (scratch.t != Value::OBJECT) {
            std::stringstream ss;
            ss << "Multi mode: Top-level object was a " << type_str(scratch.t) << ", "
               << "should be an object whose keys are filenames and values hold "
               << "the JSON for that file.";
            throw makeError(loc, ss.str());
        }
        auto *obj = static_cast<HeapObject*>(scratch.v.h);
        runInvariants(loc, obj);
        std::map<String, const Identifier*> fields;
        for (const auto &f : objectFields(obj, true)) {
            fields[f->name] = f;
        }
        for (const auto &f : fields) {
            // pushes FRAME_CALL
            const AST *body = objectIndex(loc, obj, f.second, 0);
            stack.top().val = scratch;
            evaluate(body, stack.size());
            auto vstr = string ? manifestString(body->location)
                               : manifestJson(body->location, true, U"");
            // Reset scratch so that the object we're manifesting doesn't
            // get GC'd.
            scratch = stack.top().val;
            stack.pop();
            r[encode_utf8(f.first)] = encode_utf8(vstr);
        }
        return r;
    }

    std::vector<std::string> manifestStream(void)
    {
        std::vector<std::string> r;
        LocationRange loc("During manifestation");
        if (scratch.t != Value::ARRAY) {
            std::stringstream ss;
            ss << "Stream mode: Top-level object was a " << type_str(scratch.t) << ", "
               << "should be an array whose elements hold "
               << "the JSON for each document in the stream.";
            throw makeError(loc, ss.str());
        }
        auto *arr = static_cast<HeapArray*>(scratch.v.h);
        for (auto *thunk : arr->elements) {
            LocationRange tloc = thunk->body == nullptr
                               ? loc
                               : thunk->body->location;
            if (thunk->filled) {
                stack.newCall(loc, thunk, nullptr, 0, BindingFrame{});
                // Keep arr alive when scratch is overwritten
                stack.top().val = scratch;
                scratch = thunk->content;
            } else {
                stack.newCall(loc, thunk, thunk->self, thunk->offset, thunk->upValues);
                // Keep arr alive when scratch is overwritten
                stack.top().val = scratch;
                evaluate(thunk->body, stack.size());
            }
            String element = manifestJson(tloc, true, U"");
            scratch = stack.top().val;
            stack.pop();
            r.push_back(encode_utf8(element));
        }
        return r;
    }

};

}  // namespace

std::string jsonlang_vm_execute(
    Allocator *alloc,
    const AST *ast,
    const ExtMap &ext_vars,
    unsigned max_stack,
    double gc_min_objects,
    double gc_growth_trigger,
    const VmNativeCallbackMap &natives,
    JsonlangImportCallback *import_callback,
    void *ctx,
    bool string_output)
{
    Interpreter vm(alloc, ext_vars, max_stack, gc_min_objects, gc_growth_trigger,
                   natives, import_callback, ctx);
    vm.evaluate(ast, 0);
    if (string_output) {
        return encode_utf8(vm.manifestString(LocationRange("During manifestation")));
    } else {
        return encode_utf8(vm.manifestJson(LocationRange("During manifestation"), true, U""));
    }
}

StrMap jsonlang_vm_execute_multi(
    Allocator *alloc,
    const AST *ast,
    const ExtMap &ext_vars,
    unsigned max_stack,
    double gc_min_objects,
    double gc_growth_trigger,
    const VmNativeCallbackMap &natives,
    JsonlangImportCallback *import_callback,
    void *ctx,
    bool string_output)
{
    Interpreter vm(alloc, ext_vars, max_stack, gc_min_objects, gc_growth_trigger,
                   natives, import_callback, ctx);
    vm.evaluate(ast, 0);
    return vm.manifestMulti(string_output);
}

std::vector<std::string> jsonlang_vm_execute_stream(
    Allocator *alloc,
    const AST *ast,
    const ExtMap &ext_vars,
    unsigned max_stack,
    double gc_min_objects,
    double gc_growth_trigger,
    const VmNativeCallbackMap &natives,
    JsonlangImportCallback *import_callback,
    void *ctx)
{
    Interpreter vm(alloc, ext_vars, max_stack, gc_min_objects, gc_growth_trigger,
                   natives, import_callback, ctx);
    vm.evaluate(ast, 0);
    return vm.manifestStream();
}