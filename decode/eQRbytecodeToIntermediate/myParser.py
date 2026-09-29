import struct
import ply.yacc as yacc
from .myScanner import *

class Parser:

    def __init__(self, lexer, fileName, debug=False):
        self.fileName = fileName
        self.parser = yacc.yacc(module=self, debug=debug)
        self.lexer = lexer
        self.output = open(f"{fileName}.qr", "w", encoding="utf-8")
        self.curline = 0

    # Functions to decode the binary representation of the references
    def _exponential_ones_value(self, ones: int) -> int:
        if ones == 0:
            return 0
        if ones == 4:
            return 2**ones - 1
        return self._exponential_ones_value(ones // 2) + 2**(ones // 2) - 1

    def binRefToIntRef(self, value: str) -> int:
        if len(value) > 4 and not value.startswith("1" * (len(value) // 2)):
            raise Exception("Wrong format of exponential uint")
        if len(value) == 4:
            return int(value, 2)
        return self._exponential_ones_value(len(value) // 2) + int(value[len(value) // 2:], 2)

    # Decodes the binary representation of non references integers
    def from_twos_complement_binary(self, s):
        if s[0] == '1':
            return str(int(s, 2) - (1 << len(s)))
        return str(int(s, 2))

    tokens = Scanner.tokens

    # A program is a list of the encodings of the instruction
    def p_prog(self, p):
        '''
        prog : qrtree_header op_list
        '''
        self.output.close()

    # It could be extended and rewritten to handle multiple QRtree header commands as a list
    def p_qrtree_header(self, p):
        '''
        qrtree_header : DICT_HEADER ZERO ZERO ZERO
        '''

    def p_op_list(self, p):
        '''
        op_list : op_list op
                | op
        '''

    def p_op(self, p):
        '''
        op : input
           | inputs
           | print
           | printex
           | goto
           | if
           | ifc
        '''

    def p_input(self, p):
        '''
        input : ZERO ZERO ZERO ZERO constant
              | ZERO ZERO ZERO ONE number
        '''
        if p[4] == '0':
            self.output.write("(" + str(self.curline) + ") input " + '"' + p[5] + '"' + '\n')
        else:
            val = self.binRefToIntRef(p[5])
            self.output.write("(" + str(self.curline) + ") input " + str(val) + '\n')
        self.curline += 1

    def p_inputs(self, p):
        '''
        inputs : ZERO ZERO ONE ZERO constant
               | ZERO ZERO ONE ONE number
        '''
        if p[4] == '0':
            self.output.write("(" + str(self.curline) + ") inputs " + '"' + p[5] + '"' + '\n')
        else:
            val = self.binRefToIntRef(p[5])
            self.output.write("(" + str(self.curline) + ") inputs " + str(val) + '\n')
        self.curline += 1

    def p_print(self, p):
        '''
        print : ZERO ONE ZERO ZERO constant
              | ZERO ONE ZERO ONE number
        '''
        if p[4] == '0':
            self.output.write("(" + str(self.curline) + ") print " + '"' + p[5] + '"' + '\n')
        else:
            val = self.binRefToIntRef(p[5])
            self.output.write("(" + str(self.curline) + ") print " + str(val) + '\n')
        self.curline += 1

    def p_printex(self, p):
        '''
        printex : ZERO ONE ONE ZERO constant
                | ZERO ONE ONE ONE number
        '''
        if p[4] == '0':
            self.output.write("(" + str(self.curline) + ") printex " + '"' + p[5] + '"' + '\n')
        else:
            val = self.binRefToIntRef(p[5])
            self.output.write("(" + str(self.curline) + ") printex " + str(val) + '\n')
        self.curline += 1

    def p_goto(self, p):
        '''
        goto : ONE ZERO ZERO number
        '''
        target = self.binRefToIntRef(p[4]) + self.curline + 1
        self.output.write("(" + str(self.curline) + ") goto (" + str(target) + ")" + '\n')
        self.curline += 1

    def p_if(self, p):
        '''
        if : ONE ZERO ONE ZERO constant number
           | ONE ZERO ONE ONE number number
        '''
        target = self.binRefToIntRef(p[6]) + self.curline + 1
        if p[4] == '0':
            self.output.write("(" + str(self.curline) + ") if " + '"' + p[5] + '"' + " (" + str(target) + ")" + '\n')
        else:
            ref_val = self.binRefToIntRef(p[5])
            self.output.write("(" + str(self.curline) + ") if " + str(ref_val) + " (" + str(target) + ")" + '\n')
        self.curline += 1

    def p_ifc(self, p):
        '''
        ifc : ONE ONE ZERO rel_op ZERO operand number
            | ONE ONE ZERO rel_op ONE operand number
        '''
        rel_op = p[4]
        operand_info = p[6]
        target = self.binRefToIntRef(p[7]) + self.curline + 1

        if p[5] == '0':
            val_str = self.from_twos_complement_binary(operand_info[1])
            self.output.write("(" + str(self.curline) + ") ifc " + rel_op + " " + val_str + " (" + str(target) + ")" + '\n')
        else:
            if operand_info[0] == '0':
                float_val = str(struct.unpack('!e', struct.pack('!H', int(operand_info[1], 2)))[0]) + "f16"
            else:
                float_val = str(struct.unpack('!f', struct.pack('!I', int(operand_info[1], 2)))[0]) + "f32"
            self.output.write("(" + str(self.curline) + ") ifc " + rel_op + " " + float_val + " (" + str(target) + ")" + '\n')
        self.curline += 1

    def p_operand(self, p):
        '''
        operand : optype NUMBER
        '''
        p[0] = [p[1], p[2]]

    def p_optype(self, p):
        '''
        optype : ZERO
               | ONE
        '''
        p[0] = p[1]
        if p[1] == '0':
            self.lexer.lexer.begin('n16')
        else:
            self.lexer.lexer.begin('n32')

    def p_constant(self, p):
        '''
        constant : marker4 COMPRESSED_STRING
        '''
        p[0] = p[2]

    def p_marker4(self, p):
        '''
        marker4 :
        '''
        self.lexer.lexer.begin('compressed')

    def p_number(self, p):
        '''
        number : marker ref
        '''
        p[0] = p[2]

    def p_marker(self, p):
        '''
        marker :
        '''
        self.lexer.lexer.begin('ref')

    def p_ref(self, p):
        '''
        ref : ZERO REF4
            | ONE REF4
            | ONE REF8
            | ONE REF16
            | ONE REF32
        '''
        p[0] = p[1] + p[2]

    def p_rel_op(self, p):
        '''
        rel_op : ZERO ZERO ZERO
               | ZERO ZERO ONE
               | ZERO ONE ZERO
               | ZERO ONE ONE
               | ONE ZERO ZERO
               | ONE ZERO ONE
        '''
        rel_op = "" + p[1] + p[2] + p[3]
        if rel_op == '000':
            p[0] = '=='
        elif rel_op == '001':
            p[0] = '!='
        elif rel_op == '010':
            p[0] = '<='
        elif rel_op == '011':
            p[0] = '>='
        elif rel_op == '100':
            p[0] = '<'
        elif rel_op == '101':
            p[0] = '>'

    def p_error(self, p):
        '''
        '''