/* kmalloc.h — llm.c を Pi4 からそのまま持ってくるための薄い受け皿。
 * Pi4 には専用の kmalloc があるが、Pi3 は Xinu 標準の memget/memfree を使う。
 * llm.c は load() 時に一度だけ確保して以後保持するので、これで足りる。 */
#ifndef _XINU_RPI3_KMALLOC_H_
#define _XINU_RPI3_KMALLOC_H_

#include <memory.h>

#define kmalloc(n)     memget((uint)(n))
#define kfree(p)       ((void)(p))     /* llm は解放しない（起動時に一度だけ確保） */

#endif
