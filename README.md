# QRtree compiler

The QRtree compiler is the first compiler that allows the generation of a QR code containing an executable program (which is named eQR code) and executed it.
This repository contains all the tool chain needed to compile the QRscript dialect named **QRtree** into QRtreebytecode, and then to insert the QRtreebytecode into a QR code. It also contains the tools for the inverse process to translate the QR code into QRtreebytecode and create an HTML page for the execution of the program.

# Acknowledgment

Please, cite this software (even if you just use a part of it) as:

- S. Scanzio, M. Rosani, and M. Scamuzzi, “QRtree software,” GitHub. [Online]. Available: [https://github.com/eQR-code/QRtree](https://github.com/eQR-code/QRtree)

## Dependencies

This project is written for Python 3.8 or above and is based on the following packages:

- `qrcode`, for encoding the QR code
- `ply`, for parsing the language
- `opencv-python`, for manipulating and loading the image of the QR code
- `pyzbar`, for decoding the QR code
- `argparse`, for the CLI
- `ctypes`, for loading the C library

The string compression is implemented in C (`c2_exhaustive.c`) and loaded from Python through `ctypes`. To build it you need:

- a C compiler (`gcc`) and `make` (on Windows, MinGW-w64, e.g. from MSYS2, and `mingw32-make`)

### Build the compression library

Before the first use, build the shared library with the `Makefile` placed in the same folder as `compression.py`:

```bash
make
```

This produces `libc2.so` (Linux), `libc2.dylib` (macOS) or `libc2.dll` (Windows), which has to stay in the same folder as `compression.py`. `make clean` removes it.

## Usage

The `QRtree.py` file is the main access point for both compilation and decompilation.

It requires two positional arguments:

- **action**, that can be `encode` or `decode`
- **input**, that is the path to the file to process

Other optional arguments are:

- **-d**, **--debug**, if it is present the compiler prints debug information
- **-o OUTPUT**, **--output OUTPUT**, the optional output file name, if missing the input file name is used
- **-l LANGUAGES**, **--languages LANGUAGES**, *(encode only)* comma-separated list of the language dictionaries to use for the compression, in any order (e.g. `en` or `it,en`). Default: `en`
- **-m MAX_DEPTH**, **--max-depth MAX_DEPTH**, *(encode only)* depth of the search used by the compression: `0` is the greedy search only (fastest), `N` is a search of depth `N`. Higher values can produce smaller codes at the cost of a longer compilation time. Default: `1`
- **--no-cleanup**, if present the temporary files created during the compilation are kept
- **-h**, **--help**, prints help informations

## Example

### Compile a QRtree script

For example, you write a simple test code and save it in `test.txt`.

```
print "hello world!"
```

To compile it and obtain the QR code you have to use the `encode` action.

```bash
python QRtree.py encode test.txt
```

This produces a file called `test.png` containing the QR code.

To choose the language dictionaries and the compression depth:

```bash
python QRtree.py encode test.txt -l en,it -m 2
```

### Execute a QRtree QR code

For example you have taken a picture of a QR code and saved it as `photo.png`.

To execute it you have to use the `decode` action.

```bash
python QRtree.py decode photo.png
```

This produces a file called `photo.html` and automatically opens it in the default browser of your machine starting the execution.

## Compression and language dictionaries

The strings of the program are compressed with a fragment dictionary built by a search of the most convenient fragments, and the symbols are encoded with a canonical Huffman code. The dictionary used by the program is described in the **DICT** command of the QRtree header.

A language dictionary is a statistical model of a language, shared between encoder and decoder, which is not transmitted in the QR code: the header only contains its identifier and a 16-bit fingerprint. Several dictionaries can be chained (e.g. `en,it`) and the encoder chooses the best order. Language dictionaries are resolved as follows:

- the dictionary is searched locally, in the dictionaries folder, as `<lang>.bin`
- if it is not available locally, it is downloaded from the URL configured for that language
- an unknown language falls back to English (`en`)
- if no dictionary can be resolved, the **local** mode is used: the dictionary of the symbols is sent inside the QR code, and the compression does not depend on any external file

The decoder checks the fingerprint to be sure that it is using the same dictionary used by the encoder.

The supported languages, their identifiers and their URLs are defined in `dictionaries/lang_ids.py`, which is shared by `compression.py` (encoding) and `decompression.py` (decoding).

### Create a language dictionary

A language dictionary is built from a text corpus written in that language by the script `build_language_dict.py`. For example, to create the French dictionary:

1. Go to the folder that contains `build_language_dict.py` (it also contains the `corpus` folder).
2. Put a representative text file, encoded in UTF-8, in `corpus/fr.txt`.
3. Run the script, passing the language code:

```bash
python build_dict.py fr
```

This writes `fr.bin` in the parent folder of the script, which is the folder where the encoder and the decoder look for the language dictionaries.

4. Add the new language to `dictionaries/lang_ids.py` (identifier and, optionally, the URL from which the dictionary can be downloaded), then use it with `-l fr`.

The same `fr.bin` has to be available to whoever decodes the QR code, otherwise the fingerprint check fails.

# QRtree

QRtree is a project aim at embedding a program that implements a decision three into a QR code. QRtree is one of the possibile dialects that can be embedded in a QR code, and all the possible dialects are named QR script.

The QR tree dialect was first defined in the following specifications [[2]](https://doi.org/10.48550/arXiv.2403.04708) and [[3]](https://doi.org/10.48550/arXiv.2403.04716).

# Release history

- 2024/03/13: Version v1.0 released. Contributors that worked on this edition are Stefano Scanzio, Matteo Rosani, and Mattia Scamuzzi [[3]](https://github.com/eQR-code/QRtree).

# Notes

- The eQR code was generated using the ``binary'' mode.
- In the implementation it was privilaged the maximum possible ``correction level`` at the cost of having a higher ``version``, in order to improve reliability of QR code read operations.

# Limitations

If compared with the specifications defined in [[2]](https://doi.org/10.48550/arXiv.2403.04708) and [[3]](https://doi.org/10.48550/arXiv.2403.04716).
The current version has the following limitations:

- References are encoded with an extensible format limited to the following cases 4 bits, 8 bits, 16 bits and 32 bits
- Jumps are encoded with an extensible format limited to the following cases 4 bits, 8 bits, 16 bits and 32 bits
- QRscipt header is not implemented
- When the ``--no-cleanup`` option is used, the generated file with extension .bin containing the QRbytecode binary representation of the code does not have the QRscript header (the QRtree header is written in the .bin file instead). The header is encoded in the generated eQR code, and in particular, the following sequence of bits are included: the needed padding to have the QRbytecode multiple of 8 bits, 0 for the continuation, 0000 for security, 0 for URL, 0000 for dialect, 0001 for version, 1 to indicate the presence of the QRtree header (which contains the DICT command).

# References

- [1] S. Scanzio, G. Cena and A. Valenzano, “QRscript: Embedding a Programming Language in QR codes to support Decision and Management,” 27th IEEE International Conference on Emerging Technologies and Factory Automation (ETFA 2022), 2022, pp. 1-8. doi: [10.1109/ETFA52439.2022.9921530](https://doi.org/10.1109/ETFA52439.2022.9921530) (**First paper about QRscript and QRtree**)
- [2] S. Scanzio, M. Rosani, M. Scamuzzi, and G. Cena, “QRscript specification,” arXiv, pp. 1–13, Mar. 2024. doi: [10.48550/arXiv.2403.04708](https://doi.org/10.48550/arXiv.2403.04708) (**Specification document of QRscript**)
- [3] S. Scanzio, M. Rosani, M. Scamuzzi, and G. Cena, “QRtree - Decision Tree dialect specification of QRscript,” arXiv, pp. 1–32, Mar. 2024. doi: [10.48550/arXiv.2403.04716](https://doi.org/10.48550/arXiv.2403.04716) (**Specification document of QRtree**)
- [4] S. Scanzio, M. Rosani, and M. Scamuzzi, “QRtree software,” GitHub. [Online]. Available: [https://github.com/eQR-code/QRtree](https://github.com/eQR-code/QRtree) (**This software**)

# Contributors

- [Stefano Scanzio](https://www.skenz.it/ss): was the first to propose the possibility to embed a program in a QR code, he proposed most of the programming language, and he coordinates this project
- [Matteo Rosani]: implemented the high-level language to intermediate language compiler ad the intermediate language to HTML page converter and jointly worked on the creation/reading of the QRCode
- [Mattia Scamuzzi]: implemented all the encoding and decoding part from intermediate representation to binary representation and jointly worked on the creation/reading of the QRCode
- [Stefano Gaspardone]: implemented the string compression (fragment dictionary, canonical Huffman coding, language dictionaries), the DICT command and its integration in the compiler and in the decoder
