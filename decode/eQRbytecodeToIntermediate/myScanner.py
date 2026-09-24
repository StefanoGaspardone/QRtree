import os
import ply.lex as lex
from ply.lex import TOKEN
from .decompression import load_unified_dictionaries, read_compressed_string_unified

_HERE = os.path.dirname(os.path.abspath(__file__))
DICTIONARIES_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "dictionaries"))

class Scanner:

    def __init__(self, debug=False):
        self.lexer = lex.lex(module=self, debug=debug)

    tokens = [
        'DICT_HEADER', 'ZERO', 'ONE', 'BYTE', 'NUMBER', 'REF4', 'REF8', 'REF16', 'REF32',
        'COMPRESSED_STRING',
    ]

    states = (
        ('ascii7', 'exclusive'),
        ('utf8', 'exclusive'), 
        ('n16', 'exclusive'),
        ('n32', 'exclusive'),
        ('ref', 'exclusive'),
        ('compressed', 'exclusive'),
        ('code', 'exclusive'),
    )

    def t_DICT_HEADER(self, t):
        r'011'
        lexer = t.lexer
        self.mode, self.char_info, self.suppl_info, self.frag_info, lexer.lexpos = load_unified_dictionaries(lexer.lexdata, lexer.lexpos, DICTIONARIES_DIR)
        lexer.begin('code')
        return t

    def t_code_ZERO(self,t):
        r'0'
        t.type = 'ZERO'
        return t
    def t_code_ONE(self,t):
        r'1'
        t.type = 'ONE'
        return t

    def t_ANY_eof(self,t):
        t.lexer.skip(1)

    def t_ANY_error(self,t):
        r'.'
        print("ERROR (Character not recognized): ", t.value)
        return t

    def t_ascii7_BYTE(self,t):
        r'(?!0000011)(0|1){7}'
        return t

    def t_ascii7_LOOKAHEAD(self,t):
        r'(?=0000011)'
        self.lexer.begin('code')

    def t_utf8_BYTE(self,t):
        r'(?!00000011)(0|1){8}'
        return t

    def t_utf8_LOOKAHEAD(self,t):
        r'(?=00000011)'
        self.lexer.begin('code')

    def t_n16_NUMBER(self,t):
        r'(0|1){16}'
        self.lexer.begin('code')
        return t

    def t_n32_NUMBER(self,t):
        r'(0|1){32}'
        self.lexer.begin('code')
        return t
    
    def t_ref_REF32(self,t):
        r'111111111111111(?!1111111111111111)(0|1){16}'
        self.lexer.begin('code')
        return t

    def t_ref_REF16(self,t):
        r'1111111(?!11111111)(0|1){8}'
        self.lexer.begin('code')
        return t

    def t_ref_REF8(self,t):
        r'(?<=1)111(?!1111)(0|1){4}'
        self.lexer.begin('code')
        return t

    def t_ref_REF4(self,t):
        r'(0|1){3}'
        self.lexer.begin('code')
        return t
    
    def t_compressed_COMPRESSED_STRING(self, t):
        r'[01]'
        lexer = t.lexer
        lexer.lexpos -= 1
        t.value, lexer.lexpos = read_compressed_string_unified(lexer.lexdata, lexer.lexpos, self.mode, self.char_info, self.suppl_info, self.frag_info)
        lexer.begin('code')
        return t