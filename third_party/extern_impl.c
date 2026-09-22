/* Single translation unit instantiating the header-only C dependencies. */

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
/* cgltf keeps its implementation block outside the include guard, so a second
   include (cgltf_write.h pulls cgltf.h in again) would expand it twice. */
#undef CGLTF_IMPLEMENTATION

#define CGLTF_WRITE_IMPLEMENTATION
#include "cgltf_write.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
