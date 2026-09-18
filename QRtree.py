############################################################################################################################
# QRtree Software
# This software is released under the GNU General Public License v3.0.
#
# The QRtree software is an implementation of the QRtree dialect, that allows the embedding of decision trees in a QR code.
# For more information, read the file README.md.
#
# Please, cite this software (even if you just use a part of it) as:
# S. Scanzio, M. Rosani, and M. Scamuzzi, “QRtree software,” GitHub. [Online]. Available: https://github.com/eQR-code/QRtree
############################################################################################################################

import argparse
import os
import webbrowser
import encode.HighLevelToIntermediate.main as HighLevelToIntermediate
import encode.IntermediateToeQRbytecode.main as IntermediateToeQRbytecode
import encode.eQRbytecodeToeQRcode.main as eQRbytecodeToeQRcode

import decode.eQRcodeToeQRbytecode.main as eQRcodeToeQRbytecode
import decode.eQRbytecodeToIntermediate.main as eQRbytecodeToIntermediate
import decode.IntermediateToHTML.main as IntermediateToHTML

def main(args):
    if args.type == "encode":
        main_encode(args)
    elif args.type == "decode":
        main_decode(args)
    else:
        print("Error")
    
def main_encode(args):
    HighLevelToIntermediate.encode(args.input, args.debug)
    IntermediateToeQRbytecode.encode(args.input, args.debug, args.languages, args.max_depth)
    if args.output is None:
        args.output = f"{os.path.splitext(args.input)[0]}.png"
    eQRbytecodeToeQRcode.encode(args.input, args.output)

    if not args.no_cleanup:
        os.remove(f"{os.path.splitext(args.input)[0]}.qr")
        os.remove(f"{os.path.splitext(args.input)[0]}.bin")

def main_decode(args):
    eQRcodeToeQRbytecode.decode(args.input)
    
    # Passa args.debug sia per il debug log del parser che per generare l'immagine AST
    eQRbytecodeToIntermediate.decode(args.input, debug=args.debug, generate_image=args.debug)
    
    if args.output is None:
        args.output = f"{os.path.splitext(args.input)[0]}.html"
    IntermediateToHTML.decode(args.input, args.output, args.debug)

    if not args.no_cleanup:
        os.remove(f"{os.path.splitext(args.input)[0]}.bin")
        os.remove(f"{os.path.splitext(args.input)[0]}.qr")

    # HTML page automatically opens on browser
    webbrowser.open_new(f"file://{os.path.realpath(args.output)}")

def _parse_languages(value):
    languages = [l.strip() for l in value.split(",") if l.strip()]

    if not languages:
        raise argparse.ArgumentTypeError("--languages/-l cannot be empty")

    return languages

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="QRtree Software CLI")
    parser.add_argument("type", choices=["encode", "decode"], help="Indicates the type of action")
    parser.add_argument("input", type=str, help="The input file to process")
    parser.add_argument("-o", "--output", type=str, nargs='?', help="The optional output file")
    parser.add_argument("-d", "--debug", action='store_true', help="Prints debug output and generates AST parse tree image")
    parser.add_argument("--no-cleanup", action='store_true', help="Specifies that the temporary files are not to be deleted")
    parser.add_argument("--languages", "-l", type=_parse_languages, default=["en"], help="Comma-separated list of languages to use for compression, in priority order (e.g. 'it,en')")
    parser.add_argument("--max-depth", "-m", type=int, default=1, help="DFS branch-and-bound search depth: 0 = greedy only (fastest), N = search depth N")
    
    main(parser.parse_args())