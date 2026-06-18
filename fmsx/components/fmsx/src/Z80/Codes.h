/**
    Z80: portable Z80 emulator — Codes.h

    Main table of Z80 commands. This file is included from Z80.c
    and contains the opcode dispatch logic for all Z80 instructions.

    COMPUTED GOTO AND REGISTER CACHING OPTIMIZED VERSION
    Optimized for ESP32-S3 (Xtensa) by Ivan Svarkovsky, 2026.
    Contact: ivansvarkovsky@gmail.com

    Optimizations in this version:

    - All switch/case constructs have been eliminated and replaced
     with computed goto labels (op_NAME). This removes the
     unpredictable indirect branches that cause pipeline flushes
     on the ESP32-S3 Xtensa core.
    - Local 32-bit register caching for PC and ICount: the emulated
     Z80 program counter and instruction counter are held in local
     variables throughout the dispatch loop, avoiding costly
     memory accesses on every opcode.
    - The dispatch table is arranged to maximise L1 cache hits
     for the most frequently executed opcodes.

    WARNING: This version is under active testing. Please report
    any emulation inaccuracies or crashes to:
    ivansvarkovsky@gmail.com

    This file is distributed under the same terms as the original
    Z80/fMSX code by Marat Fayzullin. Commercial distribution is
    prohibited without permission from the original author.
*/

op_JR_NZ:   if(R->AF.B.l&Z_FLAG) PC++; else { ICount-=5;M_JR; } NEXT_OP();
op_JR_NC:   if(R->AF.B.l&C_FLAG) PC++; else { ICount-=5;M_JR; } NEXT_OP();
op_JR_Z:    if(R->AF.B.l&Z_FLAG) { ICount-=5;M_JR; } else PC++; NEXT_OP();
op_JR_C:    if(R->AF.B.l&C_FLAG) { ICount-=5;M_JR; } else PC++; NEXT_OP();

op_JP_NZ:   if(R->AF.B.l&Z_FLAG) PC+=2; else { M_JP; } NEXT_OP();
op_JP_NC:   if(R->AF.B.l&C_FLAG) PC+=2; else { M_JP; } NEXT_OP();
op_JP_PO:   if(R->AF.B.l&P_FLAG) PC+=2; else { M_JP; } NEXT_OP();
op_JP_P:    if(R->AF.B.l&S_FLAG) PC+=2; else { M_JP; } NEXT_OP();
op_JP_Z:    if(R->AF.B.l&Z_FLAG) { M_JP; } else PC+=2; NEXT_OP();
op_JP_C:    if(R->AF.B.l&C_FLAG) { M_JP; } else PC+=2; NEXT_OP();
op_JP_PE:   if(R->AF.B.l&P_FLAG) { M_JP; } else PC+=2; NEXT_OP();
op_JP_M:    if(R->AF.B.l&S_FLAG) { M_JP; } else PC+=2; NEXT_OP();

op_RET_NZ:  if(!(R->AF.B.l&Z_FLAG)) { ICount-=6;M_RET; } NEXT_OP();
op_RET_NC:  if(!(R->AF.B.l&C_FLAG)) { ICount-=6;M_RET; } NEXT_OP();
op_RET_PO:  if(!(R->AF.B.l&P_FLAG)) { ICount-=6;M_RET; } NEXT_OP();
op_RET_P:   if(!(R->AF.B.l&S_FLAG)) { ICount-=6;M_RET; } NEXT_OP();
op_RET_Z:   if(R->AF.B.l&Z_FLAG)    { ICount-=6;M_RET; } NEXT_OP();
op_RET_C:   if(R->AF.B.l&C_FLAG)    { ICount-=6;M_RET; } NEXT_OP();
op_RET_PE:  if(R->AF.B.l&P_FLAG)    { ICount-=6;M_RET; } NEXT_OP();
op_RET_M:   if(R->AF.B.l&S_FLAG)    { ICount-=6;M_RET; } NEXT_OP();

op_CALL_NZ: if(R->AF.B.l&Z_FLAG) PC+=2; else { ICount-=7;M_CALL; } NEXT_OP();
op_CALL_NC: if(R->AF.B.l&C_FLAG) PC+=2; else { ICount-=7;M_CALL; } NEXT_OP();
op_CALL_PO: if(R->AF.B.l&P_FLAG) PC+=2; else { ICount-=7;M_CALL; } NEXT_OP();
op_CALL_P:  if(R->AF.B.l&S_FLAG) PC+=2; else { ICount-=7;M_CALL; } NEXT_OP();
op_CALL_Z:  if(R->AF.B.l&Z_FLAG) { ICount-=7;M_CALL; } else PC+=2; NEXT_OP();
op_CALL_C:  if(R->AF.B.l&C_FLAG) { ICount-=7;M_CALL; } else PC+=2; NEXT_OP();
op_CALL_PE: if(R->AF.B.l&P_FLAG) { ICount-=7;M_CALL; } else PC+=2; NEXT_OP();
op_CALL_M:  if(R->AF.B.l&S_FLAG) { ICount-=7;M_CALL; } else PC+=2; NEXT_OP();

op_ADD_B:    M_ADD(R->BC.B.h); NEXT_OP();
op_ADD_C:    M_ADD(R->BC.B.l); NEXT_OP();
op_ADD_D:    M_ADD(R->DE.B.h); NEXT_OP();
op_ADD_E:    M_ADD(R->DE.B.l); NEXT_OP();
op_ADD_H:    M_ADD(R->HL.B.h); NEXT_OP();
op_ADD_L:    M_ADD(R->HL.B.l); NEXT_OP();
op_ADD_A:    M_ADD(R->AF.B.h); NEXT_OP();
op_ADD_xHL:  I=RdZ80(R->HL.W);M_ADD(I); NEXT_OP();
op_ADD_BYTE: I=OpZ80(PC++);M_ADD(I); NEXT_OP();

op_SUB_B:    M_SUB(R->BC.B.h); NEXT_OP();
op_SUB_C:    M_SUB(R->BC.B.l); NEXT_OP();
op_SUB_D:    M_SUB(R->DE.B.h); NEXT_OP();
op_SUB_E:    M_SUB(R->DE.B.l); NEXT_OP();
op_SUB_H:    M_SUB(R->HL.B.h); NEXT_OP();
op_SUB_L:    M_SUB(R->HL.B.l); NEXT_OP();
op_SUB_A:    R->AF.B.h=0;R->AF.B.l=N_FLAG|Z_FLAG; NEXT_OP();
op_SUB_xHL:  I=RdZ80(R->HL.W);M_SUB(I); NEXT_OP();
op_SUB_BYTE: I=OpZ80(PC++);M_SUB(I); NEXT_OP();

op_AND_B:    M_AND(R->BC.B.h); NEXT_OP();
op_AND_C:    M_AND(R->BC.B.l); NEXT_OP();
op_AND_D:    M_AND(R->DE.B.h); NEXT_OP();
op_AND_E:    M_AND(R->DE.B.l); NEXT_OP();
op_AND_H:    M_AND(R->HL.B.h); NEXT_OP();
op_AND_L:    M_AND(R->HL.B.l); NEXT_OP();
op_AND_A:    M_AND(R->AF.B.h); NEXT_OP();
op_AND_xHL:  I=RdZ80(R->HL.W);M_AND(I); NEXT_OP();
op_AND_BYTE: I=OpZ80(PC++);M_AND(I); NEXT_OP();

op_OR_B:     M_OR(R->BC.B.h); NEXT_OP();
op_OR_C:     M_OR(R->BC.B.l); NEXT_OP();
op_OR_D:     M_OR(R->DE.B.h); NEXT_OP();
op_OR_E:     M_OR(R->DE.B.l); NEXT_OP();
op_OR_H:     M_OR(R->HL.B.h); NEXT_OP();
op_OR_L:     M_OR(R->HL.B.l); NEXT_OP();
op_OR_A:     M_OR(R->AF.B.h); NEXT_OP();
op_OR_xHL:   I=RdZ80(R->HL.W);M_OR(I); NEXT_OP();
op_OR_BYTE:  I=OpZ80(PC++);M_OR(I); NEXT_OP();

op_ADC_B:    M_ADC(R->BC.B.h); NEXT_OP();
op_ADC_C:    M_ADC(R->BC.B.l); NEXT_OP();
op_ADC_D:    M_ADC(R->DE.B.h); NEXT_OP();
op_ADC_E:    M_ADC(R->DE.B.l); NEXT_OP();
op_ADC_H:    M_ADC(R->HL.B.h); NEXT_OP();
op_ADC_L:    M_ADC(R->HL.B.l); NEXT_OP();
op_ADC_A:    M_ADC(R->AF.B.h); NEXT_OP();
op_ADC_xHL:  I=RdZ80(R->HL.W);M_ADC(I); NEXT_OP();
op_ADC_BYTE: I=OpZ80(PC++);M_ADC(I); NEXT_OP();

op_SBC_B:    M_SBC(R->BC.B.h); NEXT_OP();
op_SBC_C:    M_SBC(R->BC.B.l); NEXT_OP();
op_SBC_D:    M_SBC(R->DE.B.h); NEXT_OP();
op_SBC_E:    M_SBC(R->DE.B.l); NEXT_OP();
op_SBC_H:    M_SBC(R->HL.B.h); NEXT_OP();
op_SBC_L:    M_SBC(R->HL.B.l); NEXT_OP();
op_SBC_A:    M_SBC(R->AF.B.h); NEXT_OP();
op_SBC_xHL:  I=RdZ80(R->HL.W);M_SBC(I); NEXT_OP();
op_SBC_BYTE: I=OpZ80(PC++);M_SBC(I); NEXT_OP();

op_XOR_B:    M_XOR(R->BC.B.h); NEXT_OP();
op_XOR_C:    M_XOR(R->BC.B.l); NEXT_OP();
op_XOR_D:    M_XOR(R->DE.B.h); NEXT_OP();
op_XOR_E:    M_XOR(R->DE.B.l); NEXT_OP();
op_XOR_H:    M_XOR(R->HL.B.h); NEXT_OP();
op_XOR_L:    M_XOR(R->HL.B.l); NEXT_OP();
op_XOR_A:    R->AF.B.h=0;R->AF.B.l=P_FLAG|Z_FLAG; NEXT_OP();
op_XOR_xHL:  I=RdZ80(R->HL.W);M_XOR(I); NEXT_OP();
op_XOR_BYTE: I=OpZ80(PC++);M_XOR(I); NEXT_OP();

op_CP_B:     M_CP(R->BC.B.h); NEXT_OP();
op_CP_C:     M_CP(R->BC.B.l); NEXT_OP();
op_CP_D:     M_CP(R->DE.B.h); NEXT_OP();
op_CP_E:     M_CP(R->DE.B.l); NEXT_OP();
op_CP_H:     M_CP(R->HL.B.h); NEXT_OP();
op_CP_L:     M_CP(R->HL.B.l); NEXT_OP();
op_CP_A:     R->AF.B.l=N_FLAG|Z_FLAG; NEXT_OP();
op_CP_xHL:   I=RdZ80(R->HL.W);M_CP(I); NEXT_OP();
op_CP_BYTE:  I=OpZ80(PC++);M_CP(I); NEXT_OP();
               
op_LD_BC_WORD: M_LDWORD(BC); NEXT_OP();
op_LD_DE_WORD: M_LDWORD(DE); NEXT_OP();
op_LD_HL_WORD: M_LDWORD(HL); NEXT_OP();
op_LD_SP_WORD: M_LDWORD(SP); NEXT_OP();

op_LD_PC_HL: PC=R->HL.W;JumpZ80(PC); NEXT_OP();
op_LD_SP_HL: R->SP.W=R->HL.W; NEXT_OP();
op_LD_A_xBC: R->AF.B.h=RdZ80(R->BC.W); NEXT_OP();
op_LD_A_xDE: R->AF.B.h=RdZ80(R->DE.W); NEXT_OP();

op_ADD_HL_BC:  M_ADDW(HL,BC); NEXT_OP();
op_ADD_HL_DE:  M_ADDW(HL,DE); NEXT_OP();
op_ADD_HL_HL:  M_ADDW(HL,HL); NEXT_OP();
op_ADD_HL_SP:  M_ADDW(HL,SP); NEXT_OP();

op_DEC_BC:   R->BC.W--; NEXT_OP();
op_DEC_DE:   R->DE.W--; NEXT_OP();
op_DEC_HL:   R->HL.W--; NEXT_OP();
op_DEC_SP:   R->SP.W--; NEXT_OP();

op_INC_BC:   R->BC.W++; NEXT_OP();
op_INC_DE:   R->DE.W++; NEXT_OP();
op_INC_HL:   R->HL.W++; NEXT_OP();
op_INC_SP:   R->SP.W++; NEXT_OP();

op_DEC_B:    M_DEC(R->BC.B.h); NEXT_OP();
op_DEC_C:    M_DEC(R->BC.B.l); NEXT_OP();
op_DEC_D:    M_DEC(R->DE.B.h); NEXT_OP();
op_DEC_E:    M_DEC(R->DE.B.l); NEXT_OP();
op_DEC_H:    M_DEC(R->HL.B.h); NEXT_OP();
op_DEC_L:    M_DEC(R->HL.B.l); NEXT_OP();
op_DEC_A:    M_DEC(R->AF.B.h); NEXT_OP();
op_DEC_xHL:  I=RdZ80(R->HL.W);M_DEC(I);WrZ80(R->HL.W,I); NEXT_OP();

op_INC_B:    M_INC(R->BC.B.h); NEXT_OP();
op_INC_C:    M_INC(R->BC.B.l); NEXT_OP();
op_INC_D:    M_INC(R->DE.B.h); NEXT_OP();
op_INC_E:    M_INC(R->DE.B.l); NEXT_OP();
op_INC_H:    M_INC(R->HL.B.h); NEXT_OP();
op_INC_L:    M_INC(R->HL.B.l); NEXT_OP();
op_INC_A:    M_INC(R->AF.B.h); NEXT_OP();
op_INC_xHL:  I=RdZ80(R->HL.W);M_INC(I);WrZ80(R->HL.W,I); NEXT_OP();

op_RLCA:
  I=R->AF.B.h&0x80? C_FLAG:0;
  R->AF.B.h=(R->AF.B.h<<1)|I;
  R->AF.B.l=(R->AF.B.l&~(C_FLAG|N_FLAG|H_FLAG))|I;
  NEXT_OP();
op_RLA:
  I=R->AF.B.h&0x80? C_FLAG:0;
  R->AF.B.h=(R->AF.B.h<<1)|(R->AF.B.l&C_FLAG);
  R->AF.B.l=(R->AF.B.l&~(C_FLAG|N_FLAG|H_FLAG))|I;
  NEXT_OP();
op_RRCA:
  I=R->AF.B.h&0x01;
  R->AF.B.h=(R->AF.B.h>>1)|(I? 0x80:0);
  R->AF.B.l=(R->AF.B.l&~(C_FLAG|N_FLAG|H_FLAG))|I; 
  NEXT_OP();
op_RRA:
  I=R->AF.B.h&0x01;
  R->AF.B.h=(R->AF.B.h>>1)|(R->AF.B.l&C_FLAG? 0x80:0);
  R->AF.B.l=(R->AF.B.l&~(C_FLAG|N_FLAG|H_FLAG))|I;
  NEXT_OP();

op_RST00:    M_RST(0x0000); NEXT_OP();
op_RST08:    M_RST(0x0008); NEXT_OP();
op_RST10:    M_RST(0x0010); NEXT_OP();
op_RST18:    M_RST(0x0018); NEXT_OP();
op_RST20:    M_RST(0x0020); NEXT_OP();
op_RST28:    M_RST(0x0028); NEXT_OP();
op_RST30:    M_RST(0x0030); NEXT_OP();
op_RST38:    M_RST(0x0038); NEXT_OP();

op_PUSH_BC:  M_PUSH(BC); NEXT_OP();
op_PUSH_DE:  M_PUSH(DE); NEXT_OP();
op_PUSH_HL:  M_PUSH(HL); NEXT_OP();
op_PUSH_AF:  M_PUSH(AF); NEXT_OP();

op_POP_BC:   M_POP(BC); NEXT_OP();
op_POP_DE:   M_POP(DE); NEXT_OP();
op_POP_HL:   M_POP(HL); NEXT_OP();
op_POP_AF:   M_POP(AF); NEXT_OP();

op_DJNZ: if(--R->BC.B.h) { ICount-=5;M_JR; } else PC++; NEXT_OP();
op_JP:   M_JP; NEXT_OP();
op_JR:   M_JR; NEXT_OP();
op_CALL: M_CALL; NEXT_OP();
op_RET:  M_RET; NEXT_OP();
op_SCF:  S(C_FLAG);R(N_FLAG|H_FLAG); NEXT_OP();
op_CPL:  R->AF.B.h=~R->AF.B.h;S(N_FLAG|H_FLAG); NEXT_OP();
op_NOP:  NEXT_OP();
op_OUTA: I=OpZ80(PC++);OutZ80(I|(R->AF.W&0xFF00),R->AF.B.h); NEXT_OP();
op_INA:  I=OpZ80(PC++);R->AF.B.h=InZ80(I|(R->AF.W&0xFF00)); NEXT_OP();

op_HALT:
  PC--;
  R->IFF|=IFF_HALT;
  R->IBackup=0;
  ICount=0;
  NEXT_OP();

op_DI:
  if(R->IFF&IFF_EI) ICount+=R->IBackup-1;
  R->IFF&=~(IFF_1|IFF_2|IFF_EI);
  NEXT_OP();

op_EI:
  if(!(R->IFF&(IFF_1|IFF_EI)))
  {
    R->IFF|=IFF_2|IFF_EI;
    R->IBackup=ICount;
    ICount=1;
  }
  NEXT_OP();

op_CCF:
  R->AF.B.l^=C_FLAG;R(N_FLAG|H_FLAG);
  R->AF.B.l|=R->AF.B.l&C_FLAG? 0:H_FLAG;
  NEXT_OP();

op_EXX:
  J.W=R->BC.W;R->BC.W=R->BC1.W;R->BC1.W=J.W;
  J.W=R->DE.W;R->DE.W=R->DE1.W;R->DE1.W=J.W;
  J.W=R->HL.W;R->HL.W=R->HL1.W;R->HL1.W=J.W;
  NEXT_OP();

op_EX_DE_HL: J.W=R->DE.W;R->DE.W=R->HL.W;R->HL.W=J.W; NEXT_OP();
op_EX_AF_AF: J.W=R->AF.W;R->AF.W=R->AF1.W;R->AF1.W=J.W; NEXT_OP();
  
op_LD_B_B:   R->BC.B.h=R->BC.B.h; NEXT_OP();
op_LD_C_B:   R->BC.B.l=R->BC.B.h; NEXT_OP();
op_LD_D_B:   R->DE.B.h=R->BC.B.h; NEXT_OP();
op_LD_E_B:   R->DE.B.l=R->BC.B.h; NEXT_OP();
op_LD_H_B:   R->HL.B.h=R->BC.B.h; NEXT_OP();
op_LD_L_B:   R->HL.B.l=R->BC.B.h; NEXT_OP();
op_LD_A_B:   R->AF.B.h=R->BC.B.h; NEXT_OP();
op_LD_xHL_B: WrZ80(R->HL.W,R->BC.B.h); NEXT_OP();

op_LD_B_C:   R->BC.B.h=R->BC.B.l; NEXT_OP();
op_LD_C_C:   R->BC.B.l=R->BC.B.l; NEXT_OP();
op_LD_D_C:   R->DE.B.h=R->BC.B.l; NEXT_OP();
op_LD_E_C:   R->DE.B.l=R->BC.B.l; NEXT_OP();
op_LD_H_C:   R->HL.B.h=R->BC.B.l; NEXT_OP();
op_LD_L_C:   R->HL.B.l=R->BC.B.l; NEXT_OP();
op_LD_A_C:   R->AF.B.h=R->BC.B.l; NEXT_OP();
op_LD_xHL_C: WrZ80(R->HL.W,R->BC.B.l); NEXT_OP();

op_LD_B_D:   R->BC.B.h=R->DE.B.h; NEXT_OP();
op_LD_C_D:   R->BC.B.l=R->DE.B.h; NEXT_OP();
op_LD_D_D:   R->DE.B.h=R->DE.B.h; NEXT_OP();
op_LD_E_D:   R->DE.B.l=R->DE.B.h; NEXT_OP();
op_LD_H_D:   R->HL.B.h=R->DE.B.h; NEXT_OP();
op_LD_L_D:   R->HL.B.l=R->DE.B.h; NEXT_OP();
op_LD_A_D:   R->AF.B.h=R->DE.B.h; NEXT_OP();
op_LD_xHL_D: WrZ80(R->HL.W,R->DE.B.h); NEXT_OP();

op_LD_B_E:   R->BC.B.h=R->DE.B.l; NEXT_OP();
op_LD_C_E:   R->BC.B.l=R->DE.B.l; NEXT_OP();
op_LD_D_E:   R->DE.B.h=R->DE.B.l; NEXT_OP();
op_LD_E_E:   R->DE.B.l=R->DE.B.l; NEXT_OP();
op_LD_H_E:   R->HL.B.h=R->DE.B.l; NEXT_OP();
op_LD_L_E:   R->HL.B.l=R->DE.B.l; NEXT_OP();
op_LD_A_E:   R->AF.B.h=R->DE.B.l; NEXT_OP();
op_LD_xHL_E: WrZ80(R->HL.W,R->DE.B.l); NEXT_OP();

op_LD_B_H:   R->BC.B.h=R->HL.B.h; NEXT_OP();
op_LD_C_H:   R->BC.B.l=R->HL.B.h; NEXT_OP();
op_LD_D_H:   R->DE.B.h=R->HL.B.h; NEXT_OP();
op_LD_E_H:   R->DE.B.l=R->HL.B.h; NEXT_OP();
op_LD_H_H:   R->HL.B.h=R->HL.B.h; NEXT_OP();
op_LD_L_H:   R->HL.B.l=R->HL.B.h; NEXT_OP();
op_LD_A_H:   R->AF.B.h=R->HL.B.h; NEXT_OP();
op_LD_xHL_H: WrZ80(R->HL.W,R->HL.B.h); NEXT_OP();

op_LD_B_L:   R->BC.B.h=R->HL.B.l; NEXT_OP();
op_LD_C_L:   R->BC.B.l=R->HL.B.l; NEXT_OP();
op_LD_D_L:   R->DE.B.h=R->HL.B.l; NEXT_OP();
op_LD_E_L:   R->DE.B.l=R->HL.B.l; NEXT_OP();
op_LD_H_L:   R->HL.B.h=R->HL.B.l; NEXT_OP();
op_LD_L_L:   R->HL.B.l=R->HL.B.l; NEXT_OP();
op_LD_A_L:   R->AF.B.h=R->HL.B.l; NEXT_OP();
op_LD_xHL_L: WrZ80(R->HL.W,R->HL.B.l); NEXT_OP();

op_LD_B_A:   R->BC.B.h=R->AF.B.h; NEXT_OP();
op_LD_C_A:   R->BC.B.l=R->AF.B.h; NEXT_OP();
op_LD_D_A:   R->DE.B.h=R->AF.B.h; NEXT_OP();
op_LD_E_A:   R->DE.B.l=R->AF.B.h; NEXT_OP();
op_LD_H_A:   R->HL.B.h=R->AF.B.h; NEXT_OP();
op_LD_L_A:   R->HL.B.l=R->AF.B.h; NEXT_OP();
op_LD_A_A:   R->AF.B.h=R->AF.B.h; NEXT_OP();
op_LD_xHL_A: WrZ80(R->HL.W,R->AF.B.h); NEXT_OP();

op_LD_xBC_A: WrZ80(R->BC.W,R->AF.B.h); NEXT_OP();
op_LD_xDE_A: WrZ80(R->DE.W,R->AF.B.h); NEXT_OP();

op_LD_B_xHL:    R->BC.B.h=RdZ80(R->HL.W); NEXT_OP();
op_LD_C_xHL:    R->BC.B.l=RdZ80(R->HL.W); NEXT_OP();
op_LD_D_xHL:    R->DE.B.h=RdZ80(R->HL.W); NEXT_OP();
op_LD_E_xHL:    R->DE.B.l=RdZ80(R->HL.W); NEXT_OP();
op_LD_H_xHL:    R->HL.B.h=RdZ80(R->HL.W); NEXT_OP();
op_LD_L_xHL:    R->HL.B.l=RdZ80(R->HL.W); NEXT_OP();
op_LD_A_xHL:    R->AF.B.h=RdZ80(R->HL.W); NEXT_OP();

op_LD_B_BYTE:   R->BC.B.h=OpZ80(PC++); NEXT_OP();
op_LD_C_BYTE:   R->BC.B.l=OpZ80(PC++); NEXT_OP();
op_LD_D_BYTE:   R->DE.B.h=OpZ80(PC++); NEXT_OP();
op_LD_E_BYTE:   R->DE.B.l=OpZ80(PC++); NEXT_OP();
op_LD_H_BYTE:   R->HL.B.h=OpZ80(PC++); NEXT_OP();
op_LD_L_BYTE:   R->HL.B.l=OpZ80(PC++); NEXT_OP();
op_LD_A_BYTE:   R->AF.B.h=OpZ80(PC++); NEXT_OP();
op_LD_xHL_BYTE: WrZ80(R->HL.W,OpZ80(PC++)); NEXT_OP();

op_LD_xWORD_HL:
  J.B.l=OpZ80(PC++);
  J.B.h=OpZ80(PC++);
  WrZ80(J.W++,R->HL.B.l);
  WrZ80(J.W,R->HL.B.h);
  NEXT_OP();

op_LD_HL_xWORD:
  J.B.l=OpZ80(PC++);
  J.B.h=OpZ80(PC++);
  R->HL.B.l=RdZ80(J.W++);
  R->HL.B.h=RdZ80(J.W);
  NEXT_OP();

op_LD_A_xWORD:
  J.B.l=OpZ80(PC++);
  J.B.h=OpZ80(PC++); 
  R->AF.B.h=RdZ80(J.W);
  NEXT_OP();

op_LD_xWORD_A:
  J.B.l=OpZ80(PC++);
  J.B.h=OpZ80(PC++);
  WrZ80(J.W,R->AF.B.h);
  NEXT_OP();

op_EX_HL_xSP:
  J.B.l=RdZ80(R->SP.W);WrZ80(R->SP.W++,R->HL.B.l);
  J.B.h=RdZ80(R->SP.W);WrZ80(R->SP.W--,R->HL.B.h);
  R->HL.W=J.W;
  NEXT_OP();

op_DAA:
  J.W=R->AF.B.h;
  if(R->AF.B.l&C_FLAG) J.W|=256;
  if(R->AF.B.l&H_FLAG) J.W|=512;
  if(R->AF.B.l&N_FLAG) J.W|=1024;
  R->AF.W=DAATable[J.W];
  NEXT_OP();
  
  
