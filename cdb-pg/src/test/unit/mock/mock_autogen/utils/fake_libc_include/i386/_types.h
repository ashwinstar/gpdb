#ifndef	_BSD_I386__TYPES_H_
#define	_BSD_I386__TYPES_H_

typedef int		__int64_t;
typedef int __uint64_t;
typedef int			__darwin_intptr_t;
typedef int		__darwin_natural_t;
typedef int			__darwin_ct_rune_t;	/* ct_rune_t */
typedef union {
	char		__mbstate8[128];
	long long	_mbstateL;			/* for alignment */
} __mbstate_t;
typedef int		__darwin_mbstate_t;	/* mbstate_t */
typedef int	__darwin_ptrdiff_t;	/* ptr1 - ptr2 */
typedef int		__darwin_size_t;	/* sizeof() */
typedef int	__darwin_va_list;	/* va_list */
typedef int		__darwin_wchar_t;	/* wchar_t */
typedef int	__darwin_rune_t;	/* rune_t */
typedef int	__darwin_wint_t;	/* wint_t */
typedef int 		__darwin_clock_t;	/* clock() */
typedef int		__darwin_socklen_t;	/* socklen_t (duh) */
typedef int			__darwin_ssize_t;	/* byte count or error */
typedef int			__darwin_time_t;	/* time() */

#endif	/* _BSD_I386__TYPES_H_ */

