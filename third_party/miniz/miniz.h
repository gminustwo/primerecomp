/* (PrimeRecomp) What miniz_tinfl.c needs of miniz.h: only the inflater is vendored here - miniz_tinfl.c,
   miniz_tinfl.h and miniz_common.h, unchanged, from miniz 3.1.2 (https://github.com/richgel999/miniz, 2948eeb; MIT,
   see LICENSE). Built into runtime/nvn/inflate.c. */
#pragma once
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#include "miniz_common.h"
#include "miniz_tinfl.h"
