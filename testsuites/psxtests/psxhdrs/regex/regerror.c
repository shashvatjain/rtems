/**
 *  @file
 *  @brief regerror() API Conformance Test
 */

/*
 *  COPYRIGHT (c) 2018.
 *  Himanshu Sekhar Nayak
 *
 *  Permission to use, copy, modify, and/or distribute this software
 *  for any purpose with or without fee is hereby granted.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *  WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR
 *  BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES
 *  OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 *  WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
 *  ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <regex.h>

int test( void );

int test( void )
{
  regex_t preg;
  size_t error;
  char *regex = "\\(sim[a-z]le\\) \\1";
  char buffer[100];
  int result;

  result = regcomp( &preg, regex, REG_EXTENDED|REG_NOSUB );
  (void) result;
  error = regerror( result, &preg, buffer, 100 );

  return (error != '\0');
}
