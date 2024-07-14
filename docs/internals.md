# Internals

## Calling convention

The list of arguments are all pushed to the stack, and is re-used to be the storage space for local variables. This allows a highly efficient implementation of function calls in a stack-based VM.

All arguments are packed by respecting their alignments, similar to the layout of structs. For primitive variables and variables of struct types, the parameters are copied to the stack. For ref parameters where the callee can modify the caller's local variable, a pointer to the caller variable is pushed to the stack instead.

An example of this is shown below:

```
struct A { 
    x: double
    y: double = 1
    z: double 
}
fun f() {
    var a = A(); // This variable occupies the stack of f1()
    var b: double = 1.0;
    a.y *= 2;
    b *= 2;
    f2(a, b);
    f3(ref a, b);
}
fun pass_by_value(a: A, b: double) {
    a.y *= 2;
    b *= 2;
}
fun pass_by_ref(a: ref A, b: double) { // The variable is now a pointer to the stack memory in f1()
    a.y *= 2;
    b *= 2;
}
```

```
f():
    iconst_0        // Init var a
    iconst_0
    dconst      1.0
    iconst_0
    iconst_0
    dconst      1.0 // Init var b
    lload_0         
    dconst      2.0
    dmul
    lstore_1
    lload_0         // Value of a
    lload_1
    lload_2
    lload_3         // Value of b
    call        pass_by_value
    iconst      -6  // Address to a (dword offset)
    lload_3         // Value of b
    call        pass_by_ref

pass_by_value():
    iload_1
    dconst      2.0
    dmul
    istore_1
    iload_3
    dconst      2.0
    dmul

pass_by_ref():
    iload_0         // Load address to a
    iconst_2        // Add offset of a.y
    iadd
    idup
    lloadind        // Load a.y
    dconst      2.0
    dmul
    istoreind       // Store a.y
    iload_1         // Load value of b
    dconst      2.0
    dmul
```

## Struct memory layout

The struct layout mainly matches the C ABI - the alignments of each field are respected. 
