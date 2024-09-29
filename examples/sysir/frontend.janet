# Make a language frontend for the sysir.
# Dialect:
# TODO -
# * basic types
# * constants
# * sequence (do)
# * basic arithmetic
# * bindings
# * branch (if)
# * looping
# * returns
# * tail call returns
# * function definitions
# * arrays (declaration, loads, stores)
# * ...
# insight - using : inside symbols for types can be used to allow manipulating symbols with macros

(def slot-to-name @[])
(def name-to-slot @{})
(def type-to-name @[])
(def name-to-type @{})

(defn get-slot
  [&opt new-name]
  (def next-slot (length slot-to-name))
  (array/push slot-to-name new-name)
  (if new-name (put name-to-slot new-name next-slot))
  next-slot)

(defn named-slot
  [name]
  (assert (get name-to-slot name)))

(defn make-type
  [&opt new-name]
  (def next-type (length type-to-name))
  (array/push type-to-name new-name)
  (if new-name (put name-to-type new-name next-type))
  next-type)

(defn named-type
  [name]
  (def t (get name-to-type name))
  (assert t)
  t)

(defn setup-default-types
  [ctx]
  (def into @[])
  (defn add-prim-type
    [name native-name]
    (array/push into ~(type-prim ,name ,native-name))
    (make-type name))
  (add-prim-type 'float 'f32)
  (add-prim-type 'double 'f64)
  (add-prim-type 'int 's32)
  (add-prim-type 'long 's64)
  (add-prim-type 'ulong 'u64)
  (add-prim-type 'pointer 'pointer)
  (add-prim-type 'boolean 'boolean)
  (add-prim-type 's16 's16)
  (sysir/asm ctx into)
  ctx)

(defn type-extract
  "Given a symbol:type combination, extract the proper name and the type separately"
  [combined-name &opt default-type]
  (def parts (string/split ":" combined-name 0 2))
  (def [name tp] parts)
  [(symbol name) (symbol (or tp default-type))])

(var do-binop nil)
(var do-comp nil)

###
### Inside functions
###

(defdyn *ret-type* "Return type hint if inside function body")

(defn visit1
  "Take in a form and compile code and put it into `into`. Return result slot."
  [code into &opt no-return type-hint]
  (cond

    # Compile a constant
    (string? code) ~(pointer ,code)
    (boolean? code) ~(boolean ,code)
    (number? code) ~(,(or type-hint 'long) ,code) # TODO - should default to double

    # Needed?
    (= :core/u64 (type code)) ~(,(or type-hint 'ulong) ,code)
    (= :core/s64 (type code)) ~(,(or type-hint 'long) ,code)

    # Binding
    (symbol? code)
    (named-slot code)

    # Compile forms
    (and (tuple? code) (= :parens (tuple/type code)))
    (do
      (assert (> (length code) 0))
      (def [op & args] code)
      (case op

        # Arithmetic
        '+ (do-binop 'add args into type-hint)
        '- (do-binop 'subtract args into type-hint)
        '* (do-binop 'multiply args into type-hint)
        '/ (do-binop 'divide args into type-hint)
        '<< (do-binop 'shl args into type-hint)
        '>> (do-binop 'shr args into type-hint)

        # Comparison
        '= (do-comp 'eq args into)
        'not= (do-comp 'neq args into)
        '< (do-comp 'lt args into)
        '<= (do-comp 'lte args into)
        '> (do-comp 'gt args into)
        '>= (do-comp 'gte args into)

        # Type hinting
        'the
        (do
          (assert (= 2 (length args)))
          (def [xtype x] args)
          (def result (visit1 x into false xtype))
          (if (tuple? result) # constant
            (let [[t y] result]
              (assertf (= t xtype) "type mismatch, %p doesn't match %p" t xtype)
              [xtype y])
            (do
              (array/push into ~(bind ,result ,xtype))
              result)))

        # Named bindings
        'def
        (do
          (assert (= 2 (length args)))
          (def [full-name value] args)
          (assert (symbol? full-name))
          (def [name tp] (type-extract full-name 'int))
          (def result (visit1 value into false tp))
          (def slot (get-slot name))
          (when tp
            (array/push into ~(bind ,slot ,tp)))
          (array/push into ~(move ,slot ,result))
          slot)

        # Named variables
        'var
        (do
          (assert (= 2 (length args)))
          (def [full-name value] args)
          (assert (symbol? full-name))
          (def [name tp] (type-extract full-name 'int))
          (def result (visit1 value into false tp))
          (def slot (get-slot name))
          (when tp
            (array/push into ~(bind ,slot ,tp)))
          (array/push into ~(move ,slot ,result))
          slot)

        # Assignment
        'set
        (do
          (assert (= 2 (length args)))
          (def [to x] args)
          (def result (visit1 x into false))
          (def toslot (named-slot to))
          (array/push into ~(move ,toslot ,result))
          toslot)

        # Return
        'return
        (do
          (assert (>= 1 (length args)))
          (if (empty? args)
            (array/push into '(return))
            (do
              (def [x] args)
              (array/push into ~(return ,(visit1 x into false (dyn *ret-type*))))))
          nil)

        # Sequence of operations
        'do
        (do
          (each form (slice args 0 -2) (visit1 form into true))
          (visit1 (last args) into false type-hint))

        # While loop
        'while
        (do
          (def lab-test (keyword (gensym)))
          (def lab-exit (keyword (gensym)))
          (assert (< 1 (length args)))
          (def [cnd & body] args)
          (array/push into lab-test)
          (def condition-slot (visit1 cnd into false 'boolean))
          (array/push into ~(branch-not ,condition-slot ,lab-exit))
          (each code body
            (visit1 code into true))
          (array/push into ~(jump ,lab-test))
          (array/push into lab-exit)
          nil)

        # Branch
        'if
        (do
          (def lab (keyword (gensym)))
          (def lab-end (keyword (gensym)))
          (assert (< 2 (length args) 4))
          (def [cnd tru fal] args)
          (def condition-slot (visit1 cnd into false 'boolean))
          (def ret (get-slot))
          (array/push into ~(bind ,ret ,type-hint))
          (array/push into ~(branch ,condition-slot ,lab))
          # false path
          (array/push into ~(move ,ret ,(visit1 tru into false type-hint)))
          (array/push into ~(jump ,lab-end))
          (array/push into lab)
          # true path
          (array/push into ~(move ,ret ,(visit1 fal into false type-hint)))
          (array/push into lab-end)
          ret)

        # Insert IR
        'ir
        (do (array/push into ;args) nil)

        # Syscall
        'syscall
        (do
          (def slots @[])
          (def ret (if no-return nil (get-slot)))
          (each arg args
            (array/push slots (visit1 arg into)))
          (array/push into ~(syscall :default ,ret ,;slots))
          ret)

        # Assume function call
        (do
          (def slots @[])
          (def ret (if no-return nil (get-slot)))
          (each arg args
            (array/push slots (visit1 arg into)))
          (array/push into ~(call :default ,ret [pointer ,op] ,;slots))
          ret)))

    (errorf "cannot compile %q" code)))

(varfn do-binop
  "Emit an operation such as (+ x y).
  Extended to support any number of arguments such as (+ x y z ...)"
  [opcode args into type-hint]
  (var final nil)
  (each arg args
    (def right (visit1 arg into false type-hint))
    (set final
         (if final
           (let [result (get-slot)]
             # TODO - finish type inference - we should be able to omit the bind
             # call and sysir should be able to infer the type
             (array/push into ~(bind ,result int)) # Why int?
             (array/push into ~(,opcode ,result ,final ,right))
             result)
           right)))
  (assert final))

(varfn do-comp
  "Emit a comparison form such as (= x y z ...)"
  [opcode args into]
  (def result (get-slot))
  (def needs-temp (> 2 (length args)))
  (def temp-result (if needs-temp (get-slot) nil))
  (array/push into ~(bind ,result boolean))
  (when needs-temp
    (array/push into ~(bind ,temp-result boolean)))
  (var left nil)
  (var first-compare true)
  (each arg args
    (def right (visit1 arg into))
    (when left
      (if first-compare
        (array/push into ~(,opcode ,result ,left ,right))
        (do
          (array/push into ~(,opcode ,temp-result ,left ,right))
          (array/push into ~(and ,result ,temp-result ,result))))
      (set first-compare false))
    (set left right))
  result)

###
### Top level
###

(defn top
  "Visit and emit code for a top level form."
  [ctx form]
  (assert (tuple? form))
  (def [head & rest] form)
  (case head

    # Top level function definition
    'defn
    (do
      # TODO doc strings
      (table/clear name-to-slot)
      (array/clear slot-to-name)
      (def [name args & body] rest)
      (assert (tuple? args))
      (def [fn-name fn-tp] (type-extract name 'int))
      (def pcount (length args)) #TODO - more complicated signatures
      (def ir-asm
        @[~(link-name ,(string fn-name))
          ~(parameter-count ,pcount)])
      (each arg args
        (def [name tp] (type-extract arg 'int))
        (def slot (get-slot name))
        (array/push ir-asm ~(bind ,slot ,tp)))
      (with-dyns [*ret-type* fn-tp]
        (each part body
          (visit1 part ir-asm true)))
      # (eprintf "%.99M" ir-asm)
      (sysir/asm ctx ir-asm))

    (errorf "unknown form %v" form)))

###
### Setup
###

(def ctx (sysir/context))
(setup-default-types ctx)

(defn compile1
  [x]
  (top ctx x))

(defn dump
  []
  (eprintf "%.99M\n" (sysir/to-ir ctx)))

(defn dumpx64
  []
  (print (sysir/to-x64 ctx)))

(defn dumpx64-windows
  []
  (print (sysir/to-x64 ctx @"" :windows)))
 
(defn dumpc
  []
  (print (sysir/to-c ctx)))
