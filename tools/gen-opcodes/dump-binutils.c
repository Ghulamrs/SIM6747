#include <stdio.h>
#include <string.h>
#include "ansidecl.h"
#include "opcode/tic6x.h"
const tic6x_insn_format tic6x_insn_format_table[tic6x_insn_format_max] = {
#define FMT(name, num_bits, cst_bits, mask, fields) { num_bits, cst_bits, mask, fields },
#include "opcode/tic6x-insn-formats.h"
#undef FMT
};
static const char *fmtname[] = {
#define FMT(name, num_bits, cst_bits, mask, fields) #name,
#include "opcode/tic6x-insn-formats.h"
#undef FMT
};
const tic6x_opcode tic6x_opcode_table[tic6x_opcode_max] = {
#define INSNU(name, func_unit, format, type, isa, flags, fixed, ops, var) { STRINGX(name), CONCAT2(tic6x_func_unit_,func_unit), CONCAT3(tic6x_insn_format,_,format), CONCAT2(tic6x_pipeline_,type), CONCAT2(TIC6X_INSN_,isa), flags, fixed, ops, var },
#define INSNUE(name, e, func_unit, format, type, isa, flags, fixed, ops, var) { STRINGX(name), CONCAT2(tic6x_func_unit_,func_unit), CONCAT3(tic6x_insn_format,_,format), CONCAT2(tic6x_pipeline_,type), CONCAT2(TIC6X_INSN_,isa), flags, fixed, ops, var },
#define INSN(name, func_unit, format, type, isa, flags, fixed, ops, var) { STRINGX(name), CONCAT2(tic6x_func_unit_,func_unit), CONCAT4(tic6x_insn_format_,func_unit,_,format), CONCAT2(tic6x_pipeline_,type), CONCAT2(TIC6X_INSN_,isa), flags, fixed, ops, var },
#define INSNE(name, e, func_unit, format, type, isa, flags, fixed, ops, var) { STRINGX(name), CONCAT2(tic6x_func_unit_,func_unit), CONCAT4(tic6x_insn_format_,func_unit,_,format), CONCAT2(tic6x_pipeline_,type), CONCAT2(TIC6X_INSN_,isa), flags, fixed, ops, var },
#include "opcode/tic6x-opcode-table.h"
};
int main(void) {
  int i, j, k;
  printf("// FORMATS %d\n", (int)tic6x_insn_format_max);
  for (i = 0; i < tic6x_insn_format_max; i++) {
    const tic6x_insn_format *f = &tic6x_insn_format_table[i];
    printf("F %s %u 0x%x 0x%x %u", fmtname[i], f->num_bits, f->cst_bits, f->mask, f->num_fields);
    for (j = 0; j < (int)f->num_fields; j++) {
      const tic6x_insn_field *fl = &f->fields[j];
      int nb = fl->num_bitfields ? fl->num_bitfields : 1;
      printf(" %d:%d", fl->field_id, nb);
      for (k = 0; k < nb; k++) printf(",%u/%u/%u", fl->bitfields[k].low_pos, fl->bitfields[k].width, fl->bitfields[k].pos);
    }
    printf("\n");
  }
  for (i = 0; i < tic6x_opcode_max; i++) {
    const tic6x_opcode *o = &tic6x_opcode_table[i];
    printf("O %s %d %d %d 0x%x 0x%x %u", o->name, o->func_unit, o->format, o->type, o->isa_variants, o->flags, o->num_fixed_fields);
    for (j = 0; j < (int)o->num_fixed_fields; j++) printf(" %d/%u/%u", o->fixed_fields[j].field_id, o->fixed_fields[j].min_val, o->fixed_fields[j].max_val);
    printf(" %u", o->num_operands);
    for (j = 0; j < (int)o->num_operands; j++) { const tic6x_operand_info *p = &o->operand_info[j]; printf(" %d/%u/%d/%u/%u/%u/%u", p->form, p->size, p->rw, p->low_first, p->low_last, p->high_first, p->high_last); }
    printf(" %u", o->num_variable_fields);
    for (j = 0; j < (int)o->num_variable_fields; j++) printf(" %d/%d/%u", o->variable_fields[j].field_id, o->variable_fields[j].coding_method, o->variable_fields[j].operand_num);
    printf("\n");
  }
  return 0;
}
