/*
 * This file is part of muforth: https://muforth.nimblemachines.com/
 *
 * Copyright (c) 2002-2022 David Frech. (Read the LICENSE for details.)
 */

/* dictionary */

#include "muforth.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * Dictionary is one unified space, just like the old days. ;-)
 */

/*
 * Dictionary is kept addr-aligned. Addrs are the size of a machine
 * address: 32 bits on 32-bit machines, 64 bits on 64-bit machines.
 */
       addr *heap;  /* pointer to start of heap space */
static addr *hp;    /* heap pointer: points to next free addr in heap space */

/*
 * A struct dict_name represents what Forth folks often call a "head" - a
 * name, its length, and a link to the previous head (in the same
 * vocabulary).
 *
 * Note that this definition does -not- include the code field. All of the
 * dictionary code - except for mu_find() and init_chain() - deals only
 * with these "pure" names.
 *
 * The way that names are stored is a bit odd, but has -huge- advantages.
 * Many Forths store the link, then a byte-count-prefixed name, then
 * padding, then the code field and the rest of the body of the word. This
 * works fine when mapping from names to code fields (or bodies) - which is
 * what one does 97% of the time.
 *
 * But the other 3% of the time you're holding a code field address, or the
 * address of the body of a word, and you wonder, "What word does this
 * belong to? What's its name?" (Doing decompilation is a great example of
 * this, but not the only one.) This kind of "reverse mapping" is
 * impossible with the above "naive" dictionary layout. The problem: it's
 * impossible to reliably skip -backwards- over the name. The length byte
 * is at the -beginning- of the name, but the code field follows it.
 *
 * There are a couple of hacks that have been used. One (adopted by
 * fig-Forth) is to set the high bit of both the length byte and the last
 * byte of the name. This way one can skip backwards over the name, but
 * it's a slow scanning process, rather than an arithmetic one (subtracting
 * the length from a pointer).
 *
 * Another (ugly) hack is to put the length at -both- ends of the name.
 * This obviously makes it easy to skip over the name, forwards or
 * backwards.
 *
 * A much nicer solution is to move the link field -after- the name, and
 * forgo the prefix length entirely. This is the solution that muforth
 * adopts (as dforth did before it). Instead of -following- the link field,
 * the name -precedes- it, starting with padding, then the characters of
 * the name, then its length (one byte), followed directly by the link.
 * (The padding puts the link on an addr boundary.)
 *
 * Experienced C programmers will immediately notice a problem: structs
 * (such as the one we want to use to define the structure of dictionary
 * entries) cannot begin with a variable-length field. (In standard C they
 * cannot even -end- with variable-length fields - or that's my
 * understanding. There are evil GCC-only workarounds, however.) So, what
 * do we do? We'd like to profitably use C's structs to help write readable
 * and bug-free code, but how?
 *
 * My solution is a bit weird, but it works rather well. Since I know that
 * the characters of the name will consume at least one addr (3 or 7 bytes,
 * depending on the machine, and a length byte), I define a struct that
 * represents only the last 3 or 7 characters of the name, the length, and
 * the link; the previous characters are "off the map" as far as C is
 * concerned, but there is an easy way to address them. (To get to the
 * beginning of a name, take the address of the suffix, add the SUFFIX_LEN,
 * and subtract the length.)
 *
 * Links still point to links; adjustments are made in a couple of places
 * to convert a pointer to a link into a pointer to a name entry (struct
 * dict_name).
 */

#define FOLLOW_LINK(plink)  (addr **)(*plink)
#define MAKE_LINK(plink)    (addr *)(plink)
#define NULL_LINK           NULL

/*
 * Addrs can be 32 or 64 bits, depending on the machine; accordingly,
 * ADDR_ALIGN_SIZE is either 4 or 8.
 */
#define SUFFIX_LEN  (ADDR_ALIGN_SIZE - 1)

struct dict_name
{
    char            suffix[SUFFIX_LEN];     /* last 3 or 7 characters of name */
    unsigned char   length;                 /* 255 max; 0 means hidden */
    addr            *link;                  /* link to preceding link */
};

/*
 * A struct dict_entry is the -whole- thing: a name plus a code field.
 * Having the two together makes writing mu_find much cleaner.
 */
struct dict_entry
{
    struct dict_name    name;
    code                code;
};

/* bogus C-style dictionary init */
struct inm  /* "initial name" */
{
    char    *name;
    code    code;
};

struct inm initial_forth[] = {
#include "forth_chain.h"
    { NULL, NULL }
};

struct inm initial_compiler[] = {
#include "compiler_chain.h"
    { NULL, NULL }
};

struct inm initial_runtime[] = {
#include "runtime_chain.h"
    { NULL, NULL }
};

/* @heap pushes the heap origin address */
void mu_at_heap()    { PUSH_ADDR(heap); }

/* #heap pushes the length of the heap in bytes. */
void mu_size_heap()  { PUSH(HEAP_ADDRS * sizeof(addr)); }

void mu_here()          /* push current _value_ of heap pointer */
{
    PUSH_ADDR(hp);
}

/*
 * addr, (addr comma) copies an addr-sized pointer on the top of the stack
 * into the dictionary, and advances the heap pointer by one addr. Note
 * that hp is kept addr-aligned.
 */

void mu_addr_comma()
{
    *hp++ = POP;
}

/*
 * , (comma) copies the cell on the top of the stack into the dictionary,
 * and advances the heap pointer by one cell. Note that hp is kept
 * addr-aligned - not cell-aligned! (though this might need to change with
 * different platforms!)
 */

void mu_comma()
{
    *(cell *)hp = POP;
    hp += sizeof(cell) / sizeof(addr);
}

/*
 * allot ( n)
 *
 * Takes a count of bytes, rounds it up to an addr boundary, and adds it to
 * the heap pointer. Again, this keeps hp always addr-aligned.
 */
void mu_allot()    { hp += ADDR_ALIGNED(POP) / sizeof(addr); }

/* Align TOP to cell boundary */
void mu_aligned()       { TOP = CELL_ALIGNED(TOP); }

/* Align TOP to addr boundary */
void mu_addr_aligned()  { TOP = ADDR_ALIGNED(TOP); }

/* Type of string compare functions */
typedef int (*match_fn_t)(const char*, const char*, size_t);

/* Default at startup is case-sensitive */
match_fn_t match = strncmp;

/*
 * +case  -- make dictionary searches case-sensitive -- DEFAULT
 * -case  -- make dictionary searches case-insensitive
 */
void mu_plus_case()   { match = strncmp; }
void mu_minus_case()  { match = strncasecmp; }

/*
 * find takes a token (a u) and a chain (the head of a vocab word list) and
 * searches for the token on that chain. If found, it returns an "execution
 * token" (xt) - the address of the code field - and a true flag; if -not-
 * found, it leaves the token (a u) on the stack, and returns false.
 */
/* find  ( a u chain - a u 0 | xt -1) */
void mu_find()
{
    char *token = (char *) ST2;
    int length = ST1;
    addr **plink = (addr **)TOP;

    struct dict_entry *pde;

    /*
     * Only search if 0 < length < 256. This prevents us from matching hidden
     * entries, which have length set to 0!
     */
    if (0 < length && length < 256)
    {
        while ((plink = FOLLOW_LINK(plink)) != NULL_LINK)
        {
            /* convert pointer to link to pointer to suffix */
            pde = (struct dict_entry *)(plink - 1);

            /* for speed, don't test anything else unless lengths match */
            if (pde->name.length != length) continue;

            /* lengths match - compare strings */
            if ((*match)(pde->name.suffix + SUFFIX_LEN - length, token, length) != 0)
                continue;

            /* found: drop token, push code field address and true flag */
            DROP(1);
            ST1 = (addr)&pde->code;
            TOP = -1;
            return;
        }
    }
    /* not found: leave token, push false */
    TOP = 0;
}

#define HIDDEN 1

/*
 * new_name creates a new dictionary (name) entry and returns it
 */
static addr **new_name(
    addr **plink, char *name, int length, int hidden)
{
    struct dict_name *pnm;  /* the new name */
    int prefix_bytes;

    /*
     * Since we're using one byte to store the length, cap length at 255.
     */
    length = MIN(length, 255);

    /*
     * Calculate space for the bytes of the name that don't fit into
     * suffix[]; align to addr boundary.
     */
    prefix_bytes = ADDR_ALIGNED(length - SUFFIX_LEN);

    /*
     * Zero name + link + code. While dict started out zeroed, use of pad
     * could have put all kinds of gunk into it.
     */
    memset(hp, 0, prefix_bytes + sizeof(struct dict_entry));

    /* Compute pointer to name struct. */
    pnm = (struct dict_name *)((intptr_t)hp + prefix_bytes);

    /* Copy name string */
    memcpy(pnm->suffix + SUFFIX_LEN - length, name, length);

    /* If hidden, set length to 0 */
    pnm->length = hidden ? 0 : length;

    /* Set link pointer */
    pnm->link = MAKE_LINK(plink);

    /* Allot entry */
    hp = (addr *)(pnm + 1);

#ifdef BEING_DEFINED
    fprintf(stderr, "%p %p %.*s\n", &pnm->link, plink, length, name);
#endif

    /* Return address of link field */
    return &pnm->link;
}

/*
 * new_linked_name creates a new, non-hidden dictionary (name) entry and
 * links it onto the chain represented by plink.
 */
static void new_linked_name(
    addr **plink, char *name, int length)
{
    /* create new name & link onto front of chain */
    *plink = MAKE_LINK(new_name(FOLLOW_LINK(plink), name, length, !HIDDEN));
}

/* (linked-name)  ( a u chain) */
void mu_linked_name_()
{
    new_linked_name((addr **)TOP, (char *)ST2, ST1);
    DROP(3);
}

/*
 * Structure of dictionary chains
 *
 * A dictionary chain is a linked list of words. The head of the list is
 * the most-recently-defined word; the tail is the oldest. A chain is
 * searched from head to tail, so if a word is redefined, the newer
 * definition is the one that is found.
 *
 * The muforth core defines three chains:
 *
 *   .forth.
 *   .runtime.
 *   .compiler.
 *
 * .forth. contains most normal Forth words. In particular, it contains
 * words that are safe to execute directly while interpreting.
 *
 * .runtime. contains words that can only be executed from within a colon
 * definition.
 *
 * .compiler. contains words that are only executed at *compile time* in
 * order to compile control structures and the like.
 *
 * The user can define their own chains.
 *
 * The body of a chain word - .forth. for instance - contains a pointer
 * to the last defined word on that chain. When executed, a chain word
 * pushes the address of this pointer, which can then be searched (using
 * find) or used to set the variable current, which causes all subsequent
 * definitions to be added to that chain.
 *
 * Chains are either *sealed* or *anchored*. In a sealed chain the tail
 * link pointer is NULL. Searching a sealed chain will only find words
 * defined in that chain. In an anchored chain, the tail link pointer
 * points into another chain - the one anchored *to* - and searches of
 * the first chain will continue into the second.
 *
 * There are two ways to do this anchoring: static and dynamic. In static
 * anchoring, when chain A is anchored to chain B, at the moment that A is
 * created the head link pointer from B becomes the (initial) head link
 * pointer of A. Because of this anchoring, searches in A will continue
 * into B, and words added to A appear to extend B's list. But if words are
 * added to B *after* A is anchored to B, these will not be seen when A is
 * searched. The connection point is static.
 *
 * In dynamic anchoring, when chain A is anchored to B, instead of
 * *copying* B's head link pointer and using it as a starting point for A,
 * A links *to* B's head link pointer directly. This means that head link
 * pointers need to be more than just pointers. They need to look like
 * names, since searches will follow links *through* them. But we don't
 * ever want them to be *found*, so we create a hidden name. Since there
 * are times when we want to know when we are crossing from one chain into
 * another, we use a *special* hidden name.
 *
 * As you might have guessed, muforth uses dynamic anchoring, and the
 * special hidden name used when defining chain words is "muchain".
 *
 * .forth. is a sealed chain. When interpreting, we search .forth. and
 * execute the words found there.
 *
 * .runtime. is anchored to .forth. . When compiling, we search .runtime.
 * (which also searches .forth.), and any word found is compiled.
 *
 * .compiler. is a sealed chain. It is searched only when compiling, and
 * any word found there is *executed* rather than compiled. .compiler.
 * contains what in traditional Forths were called IMMEDIATE words.
 */

/*
 * This is the word that defines the "behaviour" of dictionary chains, the
 * same way that mu_do_colon() defines the behaviour of colon words.
 *
 * The body of a dictionary chain consists of a hidden name ("muchain")
 * followed by a link field. This link field points to the
 * most-recently-defined word on the chain. (The variable current always
 * points to the link field of *some* dictionary chain.)
 *
 * When executed, a dictionary chain pushes the address of its embedded
 * link field. This can then be used for dictionary searches, or to set
 * current (so that later definitions are added to the chain).
 */
static void mu_do_chain()
{
    /* push the address of muchain's link field by skipping over "muchain". */
    PUSH_ADDR(W + sizeof(cell)/sizeof(addr));
}

/*
 * Create a new chain.
 *
 * NOTE: This is *only* called from init_dict() in order to create the
 * initial three chains. It is never called from Forth.
 *
 * First, call new_linked_name() to create the public name: .forth. or
 * .compiler. or .runtime. . Then, populate its code field with code to
 * push the address of the following hidden word's link field. And, lastly,
 * create the hidden name ("muchain"), and return the address of the hidden
 * name's link field. We use this address to set C variables that can be
 * used by the initial C-based interpreter.
 *
 * plink points to the chain that this chain is _named_ in. It will always
 * be the .forth. chain.
 */
static addr **new_chain(
    addr **plink, char *name)
{
    new_linked_name(plink, name, strlen(name));
    *hp++ = (addr)mu_do_chain;  /* set code pointer */
    return new_name(NULL, "muchain", 7, HIDDEN);
}

/*
 * All the words defined by this function are CODE words. Their bodies are
 * defined in C; this routine compiles into the dict entry a code pointer
 * that points to the C function.
 */
static void init_chain(addr **plink, addr **anchor_link, struct inm *pinm)
{
    /*
     * Set the beginning, "anchor", link for the chain. For sealed chains,
     * this will be NULL. For chains anchored to other chains, this will be
     * address of the link field of the chain being anchored to.
     */
    *plink = MAKE_LINK(anchor_link);

    for (; pinm->name != NULL; pinm++)
    {
        new_linked_name(plink, pinm->name, strlen(pinm->name));
        *hp++ = (addr)pinm->code;   /* set code pointer */
    }
}

static void allocate()
{
    heap = (addr *)calloc(HEAP_ADDRS, sizeof(addr));

    if (heap == NULL)
        die("couldn't allocate memory");

    /* init heap pointer */
    hp = heap;
}

/*
 * These pointers and the functions to push their values are only used by
 * the initial C-based interpreter loop (see interpret.c).
 *
 * Forth code will access these chains in the normal way, using the names
 * .forth. .compiler.  and  .runtime. .
 *
 * init_dict() sets everything up.
 */
static addr **forth_chain;
static addr **compiler_chain;
static addr **runtime_chain;

void muboot_push_forth_chain()
{
    PUSH_ADDR(forth_chain);
}

void muboot_push_compiler_chain()
{
    PUSH_ADDR(compiler_chain);
}

void muboot_push_runtime_chain()
{
    PUSH_ADDR(runtime_chain);
}

void init_dict()
{
    /* we need this to "bootstrap" the .forth. chain */
    addr *forth_bootstrap = NULL_LINK;

    allocate();

    /*
     * First, create in our "bootstrap" forth chain .forth. , .compiler. ,
     * and .runtime. chains that look and smell like the ones that will
     * later be created as create/does words. These have a body that looks
     * like a normal word, but the name is always "muchain" and its length
     * byte is 0.
     */
    forth_chain    = new_chain(&forth_bootstrap, ".forth.");
    compiler_chain = new_chain(&forth_bootstrap, ".compiler.");
    runtime_chain  = new_chain(&forth_bootstrap, ".runtime.");

    /* Populate the .forth. chain with words defined in C. */
    init_chain(forth_chain, FOLLOW_LINK(&forth_bootstrap), initial_forth);

    /* Populate the .compiler. chain with words defined in C. */
    init_chain(compiler_chain, NULL_LINK, initial_compiler);

    /*
     * And we do the same with the .runtime. chain - with a wrinkle. Unlike
     * .forth. and .compiler. which are "sealed" chains, .runtime. is
     * anchored to the .forth. chain, so searches of .runtime. continue in
     * the .forth. chain.
     */
    init_chain(runtime_chain, forth_chain, initial_runtime);
}

#ifdef DEBUG_DICT
void mu_dump()
{
    fprintf(stderr, "heap origin %p\n", heap);

    /* Write the dict contents to stdout. */
    fwrite(heap, sizeof(addr), hp - heap, stdout);
    fflush(stdout);
}
#endif
