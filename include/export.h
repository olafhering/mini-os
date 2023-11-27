#ifndef _EXPORT_H_
#define _EXPORT_H_

/* Mark a symbol to be visible for apps and libs. */
#define EXPORT_SYMBOL(sym)          \
    asm(".section .export_symbol\n" \
        ".ascii \""#sym"\\n\"\n"    \
        ".previous\n")

#endif /* _EXPORT_H_ */
