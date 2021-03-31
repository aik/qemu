#include "vof.h"

int strlen(const char *s)
{
    int len = 0;

    while (*s != 0) {
        len += 1;
        s += 1;
    }

    return len;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 != 0 && *s2 != 0) {
        if (*s1 != *s2) {
            break;
        }
        s1 += 1;
        s2 += 1;
    }

    return *s1 - *s2;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    char *cdest;
    const char *csrc = src;

    cdest = dest;
    while (n-- > 0) {
        *cdest++ = *csrc++;
    }

    return dest;
}

int memcmp(const void *ptr1, const void *ptr2, size_t n)
{
    const unsigned char *p1 = ptr1;
    const unsigned char *p2 = ptr2;

    while (n-- > 0) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1 += 1;
        p2 += 1;
    }

    return 0;
}

void *memmove(void *dest, const void *src, size_t n)
{
    char *cdest;
    const char *csrc;
    int i;

    /* Do the buffers overlap in a bad way? */
    if (src < dest && src + n >= dest) {
        /* Copy from end to start */
        cdest = dest + n - 1;
        csrc = src + n - 1;
        for (i = 0; i < n; i++) {
            *cdest-- = *csrc--;
        }
    } else {
        /* Normal copy is possible */
        cdest = dest;
        csrc = src;
        for (i = 0; i < n; i++) {
            *cdest++ = *csrc++;
        }
    }

    return dest;
}

void *memset(void *dest, int c, size_t size)
{
    unsigned char *d = (unsigned char *)dest;

    while (size-- > 0) {
        *d++ = (unsigned char)c;
    }

    return dest;
}

static int toupper (int cha)
{
    if((cha >= 'a') && (cha <= 'z'))
        return(cha - 'a' + 'A');
    return(cha);
}

static unsigned long int strtoul(const char *S, char **PTR,int BASE)
{
    unsigned long rval = 0;
    short int digit;
    // *PTR is S, unless PTR is NULL, in which case i override it with my own ptr
    char* ptr;
    if (PTR == 0)
    {
        //override
        PTR = &ptr;
    }
    // i use PTR to advance through the string
    *PTR = (char *) S;
    //check if BASE is ok
    if ((BASE < 0) || BASE > 36)
    {
        return 0;
    }
    // ignore white space at beginning of S
    while ((**PTR == ' ')
           || (**PTR == '\t')
           || (**PTR == '\n')
           || (**PTR == '\r')
          )
    {
        (*PTR)++;
    }
    // if BASE is 0... determine the base from the first chars...
    if (BASE == 0)
    {
        // if S starts with "0x", BASE = 16, else 10
        if ((**PTR == '0') && (*((*PTR)+1) == 'x'))
        {
            BASE = 16;
            (*PTR)++;
            (*PTR)++;
        }
        else
            BASE = 10;
    }
    if (BASE == 16)
    {
        // S may start with "0x"
        if ((**PTR == '0') && (*((*PTR)+1) == 'x'))
        {
            (*PTR)++;
            (*PTR)++;
        }
    }
    //until end of string
    while (**PTR)
    {
        if (((**PTR) >= '0') && ((**PTR) <='9'))
        {
            //digit (0..9)
            digit = **PTR - '0';
        }
        else if (((**PTR) >= 'a') && ((**PTR) <='z'))
        {
            //alphanumeric digit lowercase(a (10) .. z (35) )
            digit = (**PTR - 'a') + 10;
        }
        else if (((**PTR) >= 'A') && ((**PTR) <='Z'))
        {
            //alphanumeric digit uppercase(a (10) .. z (35) )
            digit = (**PTR - 'A') + 10;
        }
        else
        {
            //end of parseable number reached...
            break;
        }
        if (digit < BASE)
        {
            rval = (rval * BASE) + digit;
        }
        else
        {
            //digit found, but its too big for current base
            //end of parseable number reached...
            break;
        }
        //next...
        (*PTR)++;
    }
    //done
    return rval;
}

static const unsigned long long convert[] = {
    0x0, 0xFF, 0xFFFF, 0xFFFFFF, 0xFFFFFFFF,
    0xFFFFFFFFFFULL, 0xFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL
};

static int print_str_fill(char **buffer, size_t bufsize, char *sizec,
                          const char *str, char c)
{
    int i, sizei, len;
    char *bstart = *buffer;

    sizei = strtoul(sizec, NULL, 10);
    len = strlen(str);
    if (sizei > len) {
        for (i = 0;
             (i < (sizei - len)) && ((*buffer - bstart) < bufsize);
             i++) {
            **buffer = c;
            *buffer += 1;
        }
    }
    return 1;
}

static int print_str(char **buffer, size_t bufsize, const char *str)
{
    char *bstart = *buffer;
    size_t i;

    for (i = 0; (i < strlen(str)) && ((*buffer - bstart) < bufsize); i++) {
        **buffer = str[i];
        *buffer += 1;
    }
    return 1;
}

static unsigned int print_intlen(unsigned long value, unsigned short int base)
{
    int i = 0;

    while (value > 0) {
        value /= base;
        i++;
    }
    if (i == 0)
        i = 1;
    return i;
}

static int print_itoa(char **buffer, size_t bufsize, unsigned long value,
                      unsigned short base, int upper)
{
    const char zeichen[] = "0123456789abcdef"; /* What ? */
    char c;
    size_t i, len;

    if(base <= 2 || base > 16)
        return 0;

    len = i = print_intlen(value, base);

    /* Don't print to buffer if bufsize is not enough. */
    if (len > bufsize)
        return 0;

    do {
        c = zeichen[value % base];
        if (upper)
            c = toupper(c);

        (*buffer)[--i] = c;
        value /= base;
    } while(value);

    *buffer += len;

    return 1;
}

static int print_fill(char **buffer, size_t bufsize, char *sizec, unsigned long size,
                      unsigned short int base, char c, int optlen)
{
    int i, sizei, len;
    char *bstart = *buffer;

    sizei = strtoul(sizec, NULL, 10);
    len = print_intlen(size, base) + optlen;
    if (sizei > len) {
        for (i = 0;
             (i < (sizei - len)) && ((*buffer - bstart) < bufsize);
             i++) {
            **buffer = c;
            *buffer += 1;
        }
    }

    return 0;
}

static int print_format(char **buffer, size_t bufsize, const char *format, void *var)
{
    char *start;
    unsigned int i = 0, length_mod = sizeof(int);
    unsigned long value = 0;
    unsigned long signBit;
    char *form, sizec[32];
    char sign = ' ';
    int upper = 0;

    form  = (char *) format;
    start = *buffer;

    form++;
    if(*form == '0' || *form == '.') {
        sign = '0';
        form++;
    }

    while ((*form != '\0') && ((*buffer - start) < bufsize)) {
        switch(*form) {
        case 'u':
        case 'd':
        case 'i':
            sizec[i] = '\0';
            value = (unsigned long) var;
            signBit = 0x1ULL << (length_mod * 8 - 1);
            if ((*form != 'u') && (signBit & value)) {
                **buffer = '-';
                *buffer += 1;
                value = (-(unsigned long)value) & convert[length_mod];
            }
            print_fill(buffer, bufsize - (*buffer - start),
                       sizec, value, 10, sign, 0);
            print_itoa(buffer, bufsize - (*buffer - start),
                       value, 10, upper);
            break;
        case 'X':
            upper = 1;
        case 'x':
            sizec[i] = '\0';
            value = (unsigned long) var & convert[length_mod];
            print_fill(buffer, bufsize - (*buffer - start),
                       sizec, value, 16, sign, 0);
            print_itoa(buffer, bufsize - (*buffer - start),
                       value, 16, upper);
            break;
        case 'O':
        case 'o':
            sizec[i] = '\0';
            value = (long int) var & convert[length_mod];
            print_fill(buffer, bufsize - (*buffer - start),
                       sizec, value, 8, sign, 0);
            print_itoa(buffer, bufsize - (*buffer - start),
                       value, 8, upper);
            break;
        case 'p':
            sizec[i] = '\0';
            print_fill(buffer, bufsize - (*buffer - start),
                       sizec, (unsigned long) var, 16, ' ', 2);
            print_str(buffer, bufsize - (*buffer - start),
                      "0x");
            print_itoa(buffer, bufsize - (*buffer - start),
                       (unsigned long) var, 16, upper);
            break;
        case 'c':
            sizec[i] = '\0';
            print_fill(buffer, bufsize - (*buffer - start),
                       sizec, 1, 10, ' ', 0);
            **buffer = (unsigned long) var;
            *buffer += 1;
            break;
        case 's':
            sizec[i] = '\0';
            print_str_fill(buffer,
                           bufsize - (*buffer - start), sizec,
                           (char *) var, ' ');

            print_str(buffer, bufsize - (*buffer - start),
                      (char *) var);
            break;
        case 'l':
            form++;
            if(*form == 'l') {
                length_mod = sizeof(long long int);
            } else {
                form--;
                length_mod = sizeof(long int);
            }
            break;
        case 'h':
            form++;
            if(*form == 'h') {
                length_mod = sizeof(signed char);
            } else {
                form--;
                length_mod = sizeof(short int);
            }
            break;
        case 'z':
            length_mod = sizeof(size_t);
            break;
        default:
            if(*form >= '0' && *form <= '9')
                sizec[i++] = *form;
        }
        form++;
    }

    return (long int) (*buffer - start);
}


/*
 * The vsnprintf function prints a formated strings into a buffer.
 * BUG: buffer size checking does not fully work yet
 */
int vsnprintf(char *buffer, size_t bufsize, const char *format, va_list arg)
{
    char *ptr, *bstart;

    bstart = buffer;
    ptr = (char *) format;

    /*
     * Return from here if size passed is zero, otherwise we would
     * overrun buffer while setting NULL character at the end.
     */
    if (!buffer || !bufsize)
        return 0;

    /* Leave one space for NULL character */
    bufsize--;

    while(*ptr != '\0' && (buffer - bstart) < bufsize)
    {
        if(*ptr == '%') {
            char formstr[20];
            int i=0;

            do {
                formstr[i] = *ptr;
                ptr++;
                i++;
            } while(!(*ptr == 'd' || *ptr == 'i' || *ptr == 'u' || *ptr == 'x' || *ptr == 'X'
                      || *ptr == 'p' || *ptr == 'c' || *ptr == 's' || *ptr == '%'
                      || *ptr == 'O' || *ptr == 'o' )); 
            formstr[i++] = *ptr;
            formstr[i] = '\0';
            if(*ptr == '%') {
                *buffer++ = '%';
            } else {
                print_format(&buffer,
                             bufsize - (buffer - bstart),
                             formstr, va_arg(arg, void *));
            }
            ptr++;
        } else {

            *buffer = *ptr;

            buffer++;
            ptr++;
        }
    }

    *buffer = '\0';

    return (buffer - bstart);
}

int snprintf(char *buf, int len, const char* fmt, ...)
{
    int count;
    va_list ap;

    va_start(ap, fmt);
    count = vsnprintf(buf, len, fmt, ap);
    va_end(ap);

    return count;
}

int printk(const char* fmt, ...)
{
    int count;
    va_list ap;
    char buf[256];

    va_start(ap, fmt);
    count = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    ci_stdoutn(buf, count);

    return count;
}
