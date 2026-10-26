/* Tool adapter for the framework's shared PT_SCE_RELA implementation. */

#ifndef ARMRECOMP_RELOC_H
#define ARMRECOMP_RELOC_H

#include "container.h"
#include "vitarecomp/reloc.h"

#include <stddef.h>
#include <stdint.h>

#define VR_NONE VITA_RELOC_NONE
#define VR_ABS32 VITA_RELOC_ABS32
#define VR_REL32 VITA_RELOC_REL32
#define VR_ABS8 VITA_RELOC_ABS8
#define VR_THM_CALL VITA_RELOC_THM_CALL
#define VR_CALL VITA_RELOC_CALL
#define VR_JUMP24 VITA_RELOC_JUMP24
#define VR_THM_JUMP24 VITA_RELOC_THM_JUMP24
#define VR_TARGET1 VITA_RELOC_TARGET1
#define VR_V4BX VITA_RELOC_V4BX
#define VR_TARGET2 VITA_RELOC_TARGET2
#define VR_PREL31 VITA_RELOC_PREL31
#define VR_MOVW_ABS_NC VITA_RELOC_MOVW_ABS_NC
#define VR_MOVT_ABS VITA_RELOC_MOVT_ABS
#define VR_THM_MOVW_ABS_NC VITA_RELOC_THM_MOVW_ABS_NC
#define VR_THM_MOVT_ABS VITA_RELOC_THM_MOVT_ABS
#define VR_THM_JUMP11 VITA_RELOC_THM_JUMP11
#define VR_THM_JUMP8 VITA_RELOC_THM_JUMP8
#define VR_RBASE VITA_RELOC_RBASE
#define VR_SEG_ABSOLUTE VITA_RELOC_SEGMENT_ABSOLUTE

typedef vita_reloc_entry vr_entry;
typedef vita_reloc_table vr_table;

int vr_parse(const uint8_t *buf, size_t size, const vc_module *module,
             vr_table *out, char *err, size_t errsz);
int vr_apply(uint8_t *buf, size_t size, const vc_module *module,
             const vr_table *table, uint32_t *changed,
             uint32_t *unsupported, uint32_t *outside);
void vr_free(vr_table *table);
const char *vr_code_name(uint8_t code);

#endif /* ARMRECOMP_RELOC_H */
