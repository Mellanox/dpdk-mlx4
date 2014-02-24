#!/bin/sh

# Crude script to detect whether particular types, macros and functions are
# defined by trying to compile a file with a given header. Can be used to
# perform cross-platform checks since the resulting object file isn't
# executed.
#
# Uses environment variable for CC, CPPFLAGS and CFLAGS.
#
# Always successful unless the output file cannot be written to.

file=${1:?output file name required (config.h)}
macro=${2:?output macro name required (HAVE_*)}
include=${3:?include name required (foo.h)}
type=${4:?object type required (define, enum, type, func)}
name=${5:?define/type/function name required}

: ${CC:=cc}

temp=/tmp/${0##*/}.$$.c

case $type in
define)
	code="\
#ifndef $name
#error $name not defined
#endif
"
	;;
enum)
	code="\
int test____ = $name;
"
	;;
type)
	code="\
$name test____;
"
	;;
func)
	code="\
void (*test____)() = (void (*)())$name;
"
	;;
*)
	unset error
	: ${error:?unknown object type \"$type\"}
	exit
esac

printf '+ Looking for %s %s in %s.\n' "${name}" "${type}" "${include}"

printf "\
#include <%s>

%s
" "$include" "$code" > "${temp}" &&
if ${CC} ${CPPFLAGS} ${CFLAGS} -c -o /dev/null "${temp}"
then
	rm -f "${temp}"
	printf "\
#ifndef %s
#define %s 1
#endif /* %s */

" "${macro}" "${macro}" "${macro}" >> "${file}" &&
	printf '+ Found %s in %s.\n' "${name}" "${include}"
else
	rm -f "${temp}"
	printf "\
/* %s is not defined. */

" "${macro}" >> "${file}" &&
	printf '+ Did not find %s in %s.\n' "${name}" "${include}"
fi

exit
