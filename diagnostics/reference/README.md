# Reference text

The DEC manuals the emulated devices are built from. The committed form is the
`pdftotext -layout` extract, which is what a question gets answered by grep;
the scans themselves stay out of the repository (`.gitignore`) and are fetched
from the source each section names, and read when a figure or a table's
alignment matters.

## DELQA

`EK-DELQA-UG-002.txt` is the DELQA User's Guide, OCR'd so it can be grepped.
The scan is at
`treasures.scss.tcd.ie/hardware/TCD-SCSS-T.20141120.008/EK-DELQA-UG-002.pdf`
and carries no text layer; `ocrmypdf --force-ocr` followed by `pdftotext
-layout` produces this. Read it here rather than paging through the images —
the register and status-word tables come through cleanly, and answering a
question by grep beats answering it by inference.

Section 3 holds the programming model. The parts that decide DELQA emulation
behaviour:

- 3.3.2.1 CSR bits, including the CSR08/CSR09 table that names the four
  loopback modes. The prose in 3.6.5 contradicts that table and is wrong.
- 3.4.3.1 the flag word, 3.4.3.5 the transmit and receive status words.
- 3.6.5 loopback, which states that internal loopback carries six-byte
  packets and nothing else, and that external loopback needs a connector.

DEC's CZQNA diagnostic listings are on bitsavers under
`pdf/dec/pdp11/microfiche/Diagnostic_Program_Listings/Listings/`, revisions A
through E. Those are the DEQNA-only versions; the DEQNA/DELQA/DESQA rewrite
that XXDP 2.5 carries as `ZQNAJ0.BIC` was never fiched, so it has to be read
by disassembling the binary itself.

## TS11 / TSV05

`EK-OTS11-TM-003_TS11techMan.txt` is the TS11 Technical Manual and
`EK-TSV05-UG-001_TSV05_Users_Guide_Sep82.txt` the TSV05 User's Guide. The scans
are on bitsavers under `pdf/dec/magtape/ts11/` and `pdf/dec/qbus/TSV05/`, and
both carry a text layer, so `pdftotext -layout` alone produces these extracts.
Together they are the specification the `ts11_c` emulation is built from:
the TS11 manual defines the registers, the packet protocol and the extended
status registers, and the TSV05 guide gives the Q-bus board's differences.

The parts that decide TS11 emulation behaviour:

- TS11 TM 5.1 the registers: 5.1.2 TSDB and its two maintenance byte-write
  wraparounds, 5.1.3 TSSR and the note that any write to the status address is
  a subsystem initialize, 5.1.4 the five extended status registers.
- TS11 TM 5.2 packet processing: 5.2.1 the command packet header word and the
  command/mode table 5-10, 5.2.3 the message packet header, and the buffer
  ownership rules that decide when an attention message displaces a command.
- TS11 TM 5.3.3 the termination and fatal class codes, 5.3.4 which status bits
  a command pointer load clears.
- TSV05 UG 3.3.2 the Q-bus registers, including 3.3.2.4 TSDBX — the byte at the
  high half of TSSR that carries command pointer bits 21:18 and the tape boot.
- TSV05 UG 3.3.4 the commands one by one, with the exact status each mode
  produces. Figure 3-19 settles the swap-bytes mapping, which the TS11 manual's
  prose in 5.2.1 states backwards against its own figures 5-6 and 5-7.

DEC's diagnostics for the subsystem are named in TSV05 UG table 2-2: CVTSAA
(logic test), CVTSBA (advanced logic test), CVTSCA (transport test), CVTSDA
(advanced transport test) and CVTSEA (data reliability). The two logic tests
run without a tape mounted.
