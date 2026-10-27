#ifndef VARIANT_H
#define VARIANT_H

// Build time knobs, each variant of the check executable setting some of these
// on the command line. The defaults request everything at the latest revision.

#ifndef TEST_VARIANT
#define TEST_VARIANT "full"
#endif

// -1 for no base revision tag at all.
#ifndef TEST_BASE_REVISION
#define TEST_BASE_REVISION 6
#endif

// Leave out the start and end markers.
#ifndef TEST_NO_MARKERS
#define TEST_NO_MARKERS 0
#endif

// Drop every request but the base revision tag.
#ifndef TEST_MINIMAL
#define TEST_MINIMAL 0
#endif

// Request the entry point feature, naming entry_req.
#ifndef TEST_ENTRY_REQUEST
#define TEST_ENTRY_REQUEST 1
#endif

#ifndef TEST_STACK_SIZE
#define TEST_STACK_SIZE 0x20000
#endif

// -1 for no paging mode request.
#ifndef TEST_PAGING_REV
#define TEST_PAGING_REV 1
#endif
#ifndef TEST_PAGING_MODE
#define TEST_PAGING_MODE 0
#endif
#ifndef TEST_PAGING_MIN
#define TEST_PAGING_MIN 0
#endif
#ifndef TEST_PAGING_MAX
#define TEST_PAGING_MAX 0
#endif

// -1 for no MP request.
#ifndef TEST_MP_FLAGS
#define TEST_MP_FLAGS 0
#endif

#ifndef TEST_ENTROPY_COUNT
#define TEST_ENTROPY_COUNT 16
#endif

// 0 for no flanterm init parameters request, which has Limine keep the screen.
#ifndef TEST_FLANTERM_REQUEST
#define TEST_FLANTERM_REQUEST 1
#endif

// 0 for no x86-64 Keep IOMMU request.
#ifndef TEST_KEEP_IOMMU_REQUEST
#define TEST_KEEP_IOMMU_REQUEST 1
#endif

// Revision of the module request; 1 and above carry the internal modules.
#ifndef TEST_MODULE_REV
#define TEST_MODULE_REV 1
#endif

// Which set of internal modules to declare (0 for none).
#ifndef TEST_INTERNAL_MODULES
#define TEST_INTERNAL_MODULES 0
#endif

// Declare a second request with the ID of the bootloader info request.
#ifndef TEST_DUPLICATE_REQUEST
#define TEST_DUPLICATE_REQUEST 0
#endif

// Declare a second base revision tag.
#ifndef TEST_DUPLICATE_BASE_REVISION
#define TEST_DUPLICATE_BASE_REVISION 0
#endif

// A firmware type request before the start marker, where it must be ignored.
#ifndef TEST_REQUEST_OUTSIDE_MARKERS
#define TEST_REQUEST_OUTSIDE_MARKERS 0
#endif

// A .limine_reqs section: 1 lists every request but the date at boot one, 2
// lists the bootloader info request 128 times.
#ifndef TEST_REQS_SECTION
#define TEST_REQS_SECTION 0
#endif

// Take addresses the shared object variant can only have resolved through
// its dynamic symbol table.
#ifndef TEST_SYMBOLS
#define TEST_SYMBOLS 0
#endif

// Draw the framebuffer test pattern.
#ifndef TEST_DRAW
#define TEST_DRAW 1
#endif

#endif
