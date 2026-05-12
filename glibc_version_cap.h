/* Force maximum GLIBC version to 2.17 for aarch64 compatibility */
#ifdef __aarch64__

/* Cap memcpy to 2.17 */
__asm__(".symver memcpy,memcpy@GLIBC_2.17");

/* Cap pthread symbols */
__asm__(".symver pthread_create,pthread_create@GLIBC_2.17");

#endif
