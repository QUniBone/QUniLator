/* pdp11disas.cpp: PDP-11 disassembler

 Copyright (c) 2026, Joerg Hoppe
 j_hoppe@t-online.de, www.retrocmp.com

 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 JOERG HOPPE BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 See pdp11disas.hpp for what this is and where the tables come from.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "pdp11disas.hpp"

/**********************************************************************
 * instruction set options
 **********************************************************************/

typedef struct {
	const char *name;
	unsigned option;
	const char *info;
} optiondescr_t;

static const optiondescr_t optiondescrs[] = { //
		{ "eis", pdp11disas_opt_eis, "extended arithmetic: mul div ash ashc" }, //
				{ "fis", pdp11disas_opt_fis, "floating instruction set: fadd fsub fmul fdiv" }, //
				{ "fp11", pdp11disas_opt_fp11, "FP11 floating point processor: 17xxxx" }, //
				{ "cis", pdp11disas_opt_cis, "commercial instruction set: 076xxx, l2dr, l3dr" }, //
				{ "mmu", pdp11disas_opt_mmu, "memory management: mfpi mtpi mfpd mtpd" }, //
				{ "mfps", pdp11disas_opt_mfps, "mfps mtps" }, //
				{ "spl", pdp11disas_opt_spl, "spl" }, //
				{ "sxs", pdp11disas_opt_sxs, "sxt xor sob" }, //
				{ "mark", pdp11disas_opt_mark, "mark" }, //
				{ "rtt", pdp11disas_opt_rtt, "rtt" }, //
				{ "mfpt", pdp11disas_opt_mfpt, "mfpt" }, //
				{ "csm", pdp11disas_opt_csm, "csm" }, //
				{ "tswlk", pdp11disas_opt_tswlk, "tstset wrtlck" }, //
				{ "fpd", pdp11disas_opt_fpd, "print the FP11 double precision mnemonics (clrd, muld, ...)" }, //
				{ "fpl", pdp11disas_opt_fpl, "print the FP11 long integer mnemonics (stcfl, ldclf, ...)" }, //
				{ NULL, 0, NULL } };

// shorthands for the model table
#define O_EIS	pdp11disas_opt_eis
#define O_FIS	pdp11disas_opt_fis
#define O_FPP	pdp11disas_opt_fp11
#define O_CIS	pdp11disas_opt_cis
#define O_MMU	pdp11disas_opt_mmu
#define O_MFPS	pdp11disas_opt_mfps
#define O_SPL	pdp11disas_opt_spl
#define O_SXS	pdp11disas_opt_sxs
#define O_MARK	pdp11disas_opt_mark
#define O_RTT	pdp11disas_opt_rtt
#define O_MFPT	pdp11disas_opt_mfpt
#define O_CSM	pdp11disas_opt_csm
#define O_TSWLK	pdp11disas_opt_tswlk

// the instruction sets a model is built with. The KDJ11 processors all share
// one set, as do the F11 ones.
#define OPTS_J	(O_EIS|O_FPP|O_MMU|O_SXS|O_MARK|O_RTT|O_MFPS|O_SPL|O_MFPT|O_CSM|O_TSWLK)
#define OPTS_F	(O_EIS|O_FPP|O_MMU|O_SXS|O_MARK|O_RTT|O_MFPS|O_MFPT)
#define OPTS_45	(O_EIS|O_FPP|O_MMU|O_SXS|O_MARK|O_RTT|O_SPL)

typedef struct {
	const char *name;
	unsigned options;
} modeldescr_t;

// Which model has which instruction sets: SimH pdp11_defs.h HAS_* and
// pdp11_cpumod.h SOP_*. What was *optional* on a model (the FP11 on an 11/34,
// the CIS on an 11/44, the KEV11 on an LSI-11) is not enabled here - "set fp11 1"
// switches it on.
static const modeldescr_t modeldescrs[] = { //
		{ "11/03", O_SXS | O_MARK | O_RTT | O_MFPS }, // LSI-11, KEV11 adds eis+fis
				{ "11/04", O_RTT }, //
				{ "11/05", 0 }, //
				{ "11/10", 0 }, // OEM version of the 11/05
				{ "11/15", 0 }, // OEM version of the 11/20
				{ "11/20", 0 }, // the default: the smallest instruction set
				{ "11/23", OPTS_F }, //
				{ "11/24", OPTS_F }, //
				{ "11/34", O_EIS | O_MMU | O_SXS | O_MARK | O_RTT | O_MFPS }, //
				{ "11/35", O_EIS | O_MMU | O_SXS | O_MARK | O_RTT }, // = 11/40
				{ "11/40", O_EIS | O_MMU | O_SXS | O_MARK | O_RTT }, //
				{ "11/44", OPTS_45 | O_MFPT | O_CSM }, //
				{ "11/45", OPTS_45 }, //
				{ "11/50", OPTS_45 }, // = 11/45
				{ "11/53", OPTS_J }, //
				{ "11/55", OPTS_45 }, // = 11/45
				{ "11/60", O_EIS | O_FPP | O_MMU | O_SXS | O_MARK | O_RTT }, //
				{ "11/70", OPTS_45 }, //
				{ "11/73", OPTS_J }, //
				{ "11/83", OPTS_J }, //
				{ "11/84", OPTS_J }, //
				{ "11/93", OPTS_J }, //
				{ "11/94", OPTS_J }, //
				{ "t11", O_SXS | O_RTT | O_MFPS | O_MFPT }, //
				{ NULL, 0 } };

// lower case, without '-', ' ' and a leading "pdp"
static std::string normalize_modelname(const std::string& s)
{
	std::string result;
	for (unsigned i = 0; i < s.length(); i++) {
		char c = (char) tolower((unsigned char )s[i]);
		if (c != '-' && c != ' ' && c != '\t')
			result += c;
	}
	if (result.compare(0, 3, "pdp") == 0)
		result = result.substr(3);
	return result;
}

// "11/34" matches "11/34", "1134" and "34"
static bool modelname_matches(const char *modelname, const std::string& normalized)
{
	std::string name(modelname);
	std::string noslash;
	for (unsigned i = 0; i < name.length(); i++)
		if (name[i] != '/')
			noslash += name[i];
	if (normalized == name || normalized == noslash)
		return true;
	size_t slash = name.find('/');
	if (slash != std::string::npos && normalized == name.substr(slash + 1))
		return true;
	return false;
}

pdp11disas_options_c::pdp11disas_options_c()
{
	// the smallest instruction set: nothing is silently accepted which a
	// PDP-11 could not execute
	cpu_model = "11/20";
	options = 0;
}

bool pdp11disas_options_c::set_cpu_model(const std::string& model)
{
	std::string normalized = normalize_modelname(model);
	for (unsigned i = 0; modeldescrs[i].name; i++)
		if (modelname_matches(modeldescrs[i].name, normalized)) {
			cpu_model = modeldescrs[i].name;
			options = modeldescrs[i].options;
			return true;
		}
	return false;
}

bool pdp11disas_options_c::set_option(const std::string& name, bool enable)
{
	std::string lowername = normalize_modelname(name); // does the lower casing
	for (unsigned i = 0; optiondescrs[i].name; i++)
		if (lowername == optiondescrs[i].name) {
			if (enable)
				options |= optiondescrs[i].option;
			else
				options &= ~optiondescrs[i].option;
			return true;
		}
	return false;
}

std::string pdp11disas_options_c::cpu_model_list(void)
{
	std::string result;
	for (unsigned i = 0; modeldescrs[i].name; i++) {
		if (i)
			result += " ";
		result += modeldescrs[i].name;
	}
	return result;
}

std::string pdp11disas_options_c::option_list(void)
{
	std::string result;
	for (unsigned i = 0; optiondescrs[i].name; i++) {
		if (i)
			result += " ";
		result += optiondescrs[i].name;
	}
	return result;
}

std::string pdp11disas_options_c::option_info(const std::string& name)
{
	std::string lowername = normalize_modelname(name);
	for (unsigned i = 0; optiondescrs[i].name; i++)
		if (lowername == optiondescrs[i].name)
			return std::string(optiondescrs[i].info);
	return std::string();
}

std::string pdp11disas_options_c::options_as_text(unsigned option_set)
{
	std::string result;
	for (unsigned i = 0; optiondescrs[i].name; i++)
		if (option_set & optiondescrs[i].option) {
			if (!result.empty())
				result += " ";
			result += optiondescrs[i].name;
		}
	if (result.empty())
		result = "none";
	return result;
}

std::string pdp11disas_options_c::as_text(void) const
{
	return "cpu pdp-" + cpu_model + ", options: " + options_as_text(options);
}

/**********************************************************************
 * the opcode table
 *
 * Ported from pdp11gui/common/Pdp11DisasU.pas, whose table was pulled 1:1
 * from SimH's opcode[]/opc_val[]/masks[] (PDP11/pdp11_sys.c). The order
 * matters: the first entry whose mask selects the opcode wins.
 **********************************************************************/

// how an instruction prints its operands
enum opclass_e {
	cls_npn,	// no operand: halt, movc, ...
	cls_reg,	// "opcode rn": rts, fadd, l2dr
	cls_sop,	// "opcode operand": clr, jmp, mfpi
	cls_3b,		// "opcode n", 3 bit literal: spl
	cls_6b,		// "opcode n", 6 bit literal: mark
	cls_8b,		// "opcode n", 8 bit literal: emt, trap
	cls_br,		// "opcode target": conditional branch
	cls_sob,	// "opcode rn,target": sob
	cls_dop,	// "opcode src,dst": mov, cmp, add
	cls_rsop,	// "opcode rn,operand": jsr, xor
	cls_sopr,	// "opcode operand,rn": mul, div, ash, ashc
	cls_fop,	// "opcode fltoperand": clrf, tstf
	cls_afop,	// "opcode acn,fltoperand": stf, stcfd
	cls_fopa,	// "opcode fltoperand,acn": addf, ldf, ldcdf
	cls_asop,	// "opcode acn,intoperand": stexp, stcfi
	cls_sopa	// "opcode intoperand,acn": ldexp, ldcif
};

typedef struct {
	// [0] = the F/I spelling, [1] = with "fpl", [2] = with "fpd",
	// [3] = with both. NULL falls back to [0].
	const char *name[4];
	uint16_t opval;
	uint16_t mask;
	uint8_t cls;
	unsigned required;	// instruction set options this instruction needs
} opentry_t;

#define OP(nm, opval, mask, cls, req)	{ { nm, NULL, NULL, NULL }, opval, mask, cls, req }
// f/d pair: clrf/clrd, mulf/muld, ...
#define OPD(nf, nd, opval, mask, cls, req)	{ { nf, nf, nd, nd }, opval, mask, cls, req }
// the four way f/d x i/l conversions: stcfi/stcfl/stcdi/stcdl
#define OPDL(nfi, nfl, ndi, ndl, opval, mask, cls, req)	{ { nfi, nfl, ndi, ndl }, opval, mask, cls, req }

#define M_ALL	0177777	// exact opcode
#define M_REG	0177770	// 3 bit register in bits 2..0
#define M_SOP	0177700	// 6 bit operand in bits 5..0
#define M_RSOP	0177000	// 3 bit register + 6 bit operand
#define M_DOP	0170000	// two 6 bit operands
#define M_BR	0177400	// 8 bit branch offset
#define M_FOP	0177700	// FP11 with one operand
#define M_FOPA	0177400	// FP11 with accumulator + operand

static const opentry_t optable[] = { //
		OP("halt", 0000000, M_ALL, cls_npn, 0), //
				OP("wait", 0000001, M_ALL, cls_npn, 0), //
				OP("rti", 0000002, M_ALL, cls_npn, 0), //
				OP("bpt", 0000003, M_ALL, cls_npn, 0), //
				OP("iot", 0000004, M_ALL, cls_npn, 0), //
				OP("reset", 0000005, M_ALL, cls_npn, 0), //
				OP("rtt", 0000006, M_ALL, cls_npn, O_RTT), //
				OP("mfpt", 0000007, M_ALL, cls_npn, O_MFPT), //
				OP("jmp", 0000100, M_SOP, cls_sop, 0), //
				OP("rts", 0000200, M_REG, cls_reg, 0), //
				OP("spl", 0000230, M_REG, cls_3b, O_SPL), //
				// condition codes: 000240 clears nothing, 000260 sets nothing
				OP("nop", 0000240, M_ALL, cls_npn, 0), //
				OP("clc", 0000241, M_ALL, cls_npn, 0), //
				OP("clv", 0000242, M_ALL, cls_npn, 0), //
				OP("clv clc", 0000243, M_ALL, cls_npn, 0), //
				OP("clz", 0000244, M_ALL, cls_npn, 0), //
				OP("clz clc", 0000245, M_ALL, cls_npn, 0), //
				OP("clz clv", 0000246, M_ALL, cls_npn, 0), //
				OP("clz clv clc", 0000247, M_ALL, cls_npn, 0), //
				OP("cln", 0000250, M_ALL, cls_npn, 0), //
				OP("cln clc", 0000251, M_ALL, cls_npn, 0), //
				OP("cln clv", 0000252, M_ALL, cls_npn, 0), //
				OP("cln clv clc", 0000253, M_ALL, cls_npn, 0), //
				OP("cln clz", 0000254, M_ALL, cls_npn, 0), //
				OP("cln clz clc", 0000255, M_ALL, cls_npn, 0), //
				OP("cln clz clv", 0000256, M_ALL, cls_npn, 0), //
				OP("ccc", 0000257, M_ALL, cls_npn, 0), //
				OP("nop", 0000260, M_ALL, cls_npn, 0), //
				OP("sec", 0000261, M_ALL, cls_npn, 0), //
				OP("sev", 0000262, M_ALL, cls_npn, 0), //
				OP("sev sec", 0000263, M_ALL, cls_npn, 0), //
				OP("sez", 0000264, M_ALL, cls_npn, 0), //
				OP("sez sec", 0000265, M_ALL, cls_npn, 0), //
				OP("sez sev", 0000266, M_ALL, cls_npn, 0), //
				OP("sez sev sec", 0000267, M_ALL, cls_npn, 0), //
				OP("sen", 0000270, M_ALL, cls_npn, 0), //
				OP("sen sec", 0000271, M_ALL, cls_npn, 0), //
				OP("sen sev", 0000272, M_ALL, cls_npn, 0), //
				OP("sen sev sec", 0000273, M_ALL, cls_npn, 0), //
				OP("sen sez", 0000274, M_ALL, cls_npn, 0), //
				OP("sen sez sec", 0000275, M_ALL, cls_npn, 0), //
				OP("sen sez sev", 0000276, M_ALL, cls_npn, 0), //
				OP("scc", 0000277, M_ALL, cls_npn, 0), //
				OP("swab", 0000300, M_SOP, cls_sop, 0), //
				OP("br", 0000400, M_BR, cls_br, 0), //
				OP("bne", 0001000, M_BR, cls_br, 0), //
				OP("beq", 0001400, M_BR, cls_br, 0), //
				OP("bge", 0002000, M_BR, cls_br, 0), //
				OP("blt", 0002400, M_BR, cls_br, 0), //
				OP("bgt", 0003000, M_BR, cls_br, 0), //
				OP("ble", 0003400, M_BR, cls_br, 0), //
				OP("jsr", 0004000, M_RSOP, cls_rsop, 0), //
				OP("clr", 0005000, M_SOP, cls_sop, 0), //
				OP("com", 0005100, M_SOP, cls_sop, 0), //
				OP("inc", 0005200, M_SOP, cls_sop, 0), //
				OP("dec", 0005300, M_SOP, cls_sop, 0), //
				OP("neg", 0005400, M_SOP, cls_sop, 0), //
				OP("adc", 0005500, M_SOP, cls_sop, 0), //
				OP("sbc", 0005600, M_SOP, cls_sop, 0), //
				OP("tst", 0005700, M_SOP, cls_sop, 0), //
				OP("ror", 0006000, M_SOP, cls_sop, 0), //
				OP("rol", 0006100, M_SOP, cls_sop, 0), //
				OP("asr", 0006200, M_SOP, cls_sop, 0), //
				OP("asl", 0006300, M_SOP, cls_sop, 0), //
				OP("mark", 0006400, M_SOP, cls_6b, O_MARK), //
				OP("mfpi", 0006500, M_SOP, cls_sop, O_MMU), //
				OP("mtpi", 0006600, M_SOP, cls_sop, O_MMU), //
				OP("sxt", 0006700, M_SOP, cls_sop, O_SXS), //
				OP("csm", 0007000, M_SOP, cls_sop, O_CSM), //
				OP("tstset", 0007200, M_SOP, cls_sop, O_TSWLK), //
				OP("wrtlck", 0007300, M_SOP, cls_sop, O_TSWLK), //
				OP("mov", 0010000, M_DOP, cls_dop, 0), //
				OP("cmp", 0020000, M_DOP, cls_dop, 0), //
				OP("bit", 0030000, M_DOP, cls_dop, 0), //
				OP("bic", 0040000, M_DOP, cls_dop, 0), //
				OP("bis", 0050000, M_DOP, cls_dop, 0), //
				OP("add", 0060000, M_DOP, cls_dop, 0), //
				OP("mul", 0070000, M_RSOP, cls_sopr, O_EIS), //
				OP("div", 0071000, M_RSOP, cls_sopr, O_EIS), //
				OP("ash", 0072000, M_RSOP, cls_sopr, O_EIS), //
				OP("ashc", 0073000, M_RSOP, cls_sopr, O_EIS), //
				OP("xor", 0074000, M_RSOP, cls_rsop, O_SXS), //
				OP("fadd", 0075000, M_REG, cls_reg, O_FIS), //
				OP("fsub", 0075010, M_REG, cls_reg, O_FIS), //
				OP("fmul", 0075020, M_REG, cls_reg, O_FIS), //
				OP("fdiv", 0075030, M_REG, cls_reg, O_FIS), //
				// commercial instruction set. The "i" forms carry their
				// descriptors as data words behind the opcode, see the header.
				OP("l2dr", 0076020, M_REG, cls_reg, O_CIS), //
				OP("movc", 0076030, M_ALL, cls_npn, O_CIS), //
				OP("movrc", 0076031, M_ALL, cls_npn, O_CIS), //
				OP("movtc", 0076032, M_ALL, cls_npn, O_CIS), //
				OP("locc", 0076040, M_ALL, cls_npn, O_CIS), //
				OP("skpc", 0076041, M_ALL, cls_npn, O_CIS), //
				OP("scanc", 0076042, M_ALL, cls_npn, O_CIS), //
				OP("spanc", 0076043, M_ALL, cls_npn, O_CIS), //
				OP("cmpc", 0076044, M_ALL, cls_npn, O_CIS), //
				OP("matc", 0076045, M_ALL, cls_npn, O_CIS), //
				OP("addn", 0076050, M_ALL, cls_npn, O_CIS), //
				OP("subn", 0076051, M_ALL, cls_npn, O_CIS), //
				OP("cmpn", 0076052, M_ALL, cls_npn, O_CIS), //
				OP("cvtnl", 0076053, M_ALL, cls_npn, O_CIS), //
				OP("cvtpn", 0076054, M_ALL, cls_npn, O_CIS), //
				OP("cvtnp", 0076055, M_ALL, cls_npn, O_CIS), //
				OP("ashn", 0076056, M_ALL, cls_npn, O_CIS), //
				OP("cvtln", 0076057, M_ALL, cls_npn, O_CIS), //
				OP("l3dr", 0076060, M_REG, cls_reg, O_CIS), //
				OP("addp", 0076070, M_ALL, cls_npn, O_CIS), //
				OP("subp", 0076071, M_ALL, cls_npn, O_CIS), //
				OP("cmpp", 0076072, M_ALL, cls_npn, O_CIS), //
				OP("cvtpl", 0076073, M_ALL, cls_npn, O_CIS), //
				OP("mulp", 0076074, M_ALL, cls_npn, O_CIS), //
				OP("divp", 0076075, M_ALL, cls_npn, O_CIS), //
				OP("ashp", 0076076, M_ALL, cls_npn, O_CIS), //
				OP("cvtlp", 0076077, M_ALL, cls_npn, O_CIS), //
				OP("movci", 0076130, M_ALL, cls_npn, O_CIS), //
				OP("movrci", 0076131, M_ALL, cls_npn, O_CIS), //
				OP("movtci", 0076132, M_ALL, cls_npn, O_CIS), //
				OP("locci", 0076140, M_ALL, cls_npn, O_CIS), //
				OP("skpci", 0076141, M_ALL, cls_npn, O_CIS), //
				OP("scanci", 0076142, M_ALL, cls_npn, O_CIS), //
				OP("spanci", 0076143, M_ALL, cls_npn, O_CIS), //
				OP("cmpci", 0076144, M_ALL, cls_npn, O_CIS), //
				OP("matci", 0076145, M_ALL, cls_npn, O_CIS), //
				OP("addni", 0076150, M_ALL, cls_npn, O_CIS), //
				OP("subni", 0076151, M_ALL, cls_npn, O_CIS), //
				OP("cmpni", 0076152, M_ALL, cls_npn, O_CIS), //
				OP("cvtnli", 0076153, M_ALL, cls_npn, O_CIS), //
				OP("cvtpni", 0076154, M_ALL, cls_npn, O_CIS), //
				OP("cvtnpi", 0076155, M_ALL, cls_npn, O_CIS), //
				OP("ashni", 0076156, M_ALL, cls_npn, O_CIS), //
				OP("cvtlni", 0076157, M_ALL, cls_npn, O_CIS), //
				OP("addpi", 0076170, M_ALL, cls_npn, O_CIS), //
				OP("subpi", 0076171, M_ALL, cls_npn, O_CIS), //
				OP("cmppi", 0076172, M_ALL, cls_npn, O_CIS), //
				OP("cvtpli", 0076173, M_ALL, cls_npn, O_CIS), //
				OP("mulpi", 0076174, M_ALL, cls_npn, O_CIS), //
				OP("divpi", 0076175, M_ALL, cls_npn, O_CIS), //
				OP("ashpi", 0076176, M_ALL, cls_npn, O_CIS), //
				OP("cvtlpi", 0076177, M_ALL, cls_npn, O_CIS), //
				OP("sob", 0077000, M_RSOP, cls_sob, O_SXS), //
				OP("bpl", 0100000, M_BR, cls_br, 0), //
				OP("bmi", 0100400, M_BR, cls_br, 0), //
				OP("bhi", 0101000, M_BR, cls_br, 0), //
				OP("blos", 0101400, M_BR, cls_br, 0), //
				OP("bvc", 0102000, M_BR, cls_br, 0), //
				OP("bvs", 0102400, M_BR, cls_br, 0), //
				OP("bcc", 0103000, M_BR, cls_br, 0), //
				OP("bcs", 0103400, M_BR, cls_br, 0), //
				OP("emt", 0104000, M_BR, cls_8b, 0), //
				OP("trap", 0104400, M_BR, cls_8b, 0), //
				OP("clrb", 0105000, M_SOP, cls_sop, 0), //
				OP("comb", 0105100, M_SOP, cls_sop, 0), //
				OP("incb", 0105200, M_SOP, cls_sop, 0), //
				OP("decb", 0105300, M_SOP, cls_sop, 0), //
				OP("negb", 0105400, M_SOP, cls_sop, 0), //
				OP("adcb", 0105500, M_SOP, cls_sop, 0), //
				OP("sbcb", 0105600, M_SOP, cls_sop, 0), //
				OP("tstb", 0105700, M_SOP, cls_sop, 0), //
				OP("rorb", 0106000, M_SOP, cls_sop, 0), //
				OP("rolb", 0106100, M_SOP, cls_sop, 0), //
				OP("asrb", 0106200, M_SOP, cls_sop, 0), //
				OP("aslb", 0106300, M_SOP, cls_sop, 0), //
				OP("mtps", 0106400, M_SOP, cls_sop, O_MFPS), //
				OP("mfpd", 0106500, M_SOP, cls_sop, O_MMU), //
				OP("mtpd", 0106600, M_SOP, cls_sop, O_MMU), //
				OP("mfps", 0106700, M_SOP, cls_sop, O_MFPS), //
				OP("movb", 0110000, M_DOP, cls_dop, 0), //
				OP("cmpb", 0120000, M_DOP, cls_dop, 0), //
				OP("bitb", 0130000, M_DOP, cls_dop, 0), //
				OP("bicb", 0140000, M_DOP, cls_dop, 0), //
				OP("bisb", 0150000, M_DOP, cls_dop, 0), //
				OP("sub", 0160000, M_DOP, cls_dop, 0), //
				// FP11 floating point processor
				OP("cfcc", 0170000, M_ALL, cls_npn, O_FPP), //
				OP("setf", 0170001, M_ALL, cls_npn, O_FPP), //
				OP("seti", 0170002, M_ALL, cls_npn, O_FPP), //
				OP("setd", 0170011, M_ALL, cls_npn, O_FPP), //
				OP("setl", 0170012, M_ALL, cls_npn, O_FPP), //
				OP("ldfps", 0170100, M_FOP, cls_sop, O_FPP), //
				OP("stfps", 0170200, M_FOP, cls_sop, O_FPP), //
				OP("stst", 0170300, M_FOP, cls_sop, O_FPP), //
				OPD("clrf", "clrd", 0170400, M_FOP, cls_fop, O_FPP), //
				OPD("tstf", "tstd", 0170500, M_FOP, cls_fop, O_FPP), //
				OPD("absf", "absd", 0170600, M_FOP, cls_fop, O_FPP), //
				OPD("negf", "negd", 0170700, M_FOP, cls_fop, O_FPP), //
				OPD("mulf", "muld", 0171000, M_FOPA, cls_fopa, O_FPP), //
				OPD("modf", "modd", 0171400, M_FOPA, cls_fopa, O_FPP), //
				OPD("addf", "addd", 0172000, M_FOPA, cls_fopa, O_FPP), //
				OPD("ldf", "ldd", 0172400, M_FOPA, cls_fopa, O_FPP), //
				OPD("subf", "subd", 0173000, M_FOPA, cls_fopa, O_FPP), //
				OPD("cmpf", "cmpd", 0173400, M_FOPA, cls_fopa, O_FPP), //
				OPD("stf", "std", 0174000, M_FOPA, cls_afop, O_FPP), //
				OPD("divf", "divd", 0174400, M_FOPA, cls_fopa, O_FPP), //
				OP("stexp", 0175000, M_FOPA, cls_asop, O_FPP), //
				OPDL("stcfi", "stcfl", "stcdi", "stcdl", 0175400, M_FOPA, cls_asop, O_FPP), //
				OPD("stcfd", "stcdf", 0176000, M_FOPA, cls_afop, O_FPP), //
				OP("ldexp", 0176400, M_FOPA, cls_sopa, O_FPP), //
				OPDL("ldcif", "ldclf", "ldcid", "ldcld", 0177000, M_FOPA, cls_sopa, O_FPP), //
				OPD("ldcfd", "ldcdf", 0177400, M_FOPA, cls_fopa, O_FPP) //
		};

#define OPTABLE_COUNT	(sizeof(optable) / sizeof(optable[0]))

/**********************************************************************
 * the well known addresses
 *
 * The trap/interrupt vectors and the I/O page. Sources: the DEC processor
 * handbooks for the processor and memory management registers, the KT11-D
 * implementation in 10.02_devices/2_src/cpu34/kt11d.c for the PAR/PDR
 * blocks, the register sets of the emulations in 10.02_devices/2_src for
 * the devices QUniBone has, and SimH's autoconfigure table
 * (PDP11/pdp11_io_lib.c auto_tab[], itself taken from the VMS SYSGEN
 * table) for the standard CSR addresses and vectors of the rest.
 *
 * Only *registers* are listed, no ROM or memory ranges: those would put a
 * comment on every second line of a listing of the code inside them.
 **********************************************************************/

typedef struct {
	uint16_t startaddr;
	unsigned count;		// consecutive registers this entry covers
	unsigned step;		// address distance between them
	const char *name;	// a "%u" in it is replaced by the index in the block
} addrdescr_t;

// Trap and interrupt vectors. Each is two words: new PC, new PSW.
static const addrdescr_t vectordescrs[] = { //
		{ 0000000, 1, 2, "reserved vector" }, //
				{ 0000004, 1, 2, "bus error, odd address, stack limit vector" }, //
				{ 0000010, 1, 2, "reserved instruction vector" }, //
				{ 0000014, 1, 2, "bpt / trace trap vector" }, //
				{ 0000020, 1, 2, "iot vector" }, //
				{ 0000024, 1, 2, "power fail vector" }, //
				{ 0000030, 1, 2, "emt vector" }, //
				{ 0000034, 1, 2, "trap vector" }, //
				{ 0000040, 4, 4, "system software vector %u" }, //
				{ 0000060, 1, 2, "console dl11 receiver vector" }, //
				{ 0000064, 1, 2, "console dl11 transmitter vector" }, //
				{ 0000070, 1, 2, "pc11 paper tape reader vector" }, //
				{ 0000074, 1, 2, "pc11 paper tape punch vector" }, //
				{ 0000100, 1, 2, "kw11-l line clock vector" }, //
				{ 0000104, 1, 2, "kw11-p programmable clock vector" }, //
				{ 0000114, 1, 2, "memory parity error vector" }, //
				{ 0000120, 1, 2, "deuna/deqna ethernet vector" }, //
				{ 0000124, 1, 2, "dr11-b vector" }, //
				{ 0000160, 1, 2, "rl11 vector" }, //
				{ 0000170, 2, 4, "lp11 line printer vector %u" }, //
				{ 0000200, 1, 2, "lp11 line printer vector" }, //
				{ 0000204, 1, 2, "rf11 fixed head disk / rh11 vector" }, //
				{ 0000210, 1, 2, "rc11 / rk611 vector" }, //
				{ 0000214, 1, 2, "tc11 dectape vector" }, //
				{ 0000220, 1, 2, "rk11 disk vector" }, //
				{ 0000224, 1, 2, "tm11 magtape / rh11 vector" }, //
				{ 0000230, 1, 2, "cr11 card reader vector" }, //
				{ 0000240, 1, 2, "pirq program interrupt request vector" }, //
				{ 0000244, 1, 2, "fp11 floating point exception vector" }, //
				{ 0000250, 1, 2, "memory management abort vector" }, //
				{ 0000254, 1, 2, "rh11 / rp11 disk vector" }, //
				{ 0000260, 1, 2, "ta11 cassette / tqk50 vector" }, //
				{ 0000264, 1, 2, "rx11 / rx211 floppy vector" }, //
				{ 0000270, 2, 4, "lp11 line printer vector %u" }, //
				{ 0, 0, 0, NULL } };

// The I/O page, 160000..177777.
static const addrdescr_t iopagedescrs[] = { //
		{ 0172150, 1, 2, "uda50/rqdx mscp ip (init and poll)" }, //
				{ 0172152, 1, 2, "uda50/rqdx mscp sa (status and address)" }, //
				{ 0172200, 8, 2, "supervisor i-space pdr %u" }, //
				{ 0172220, 8, 2, "supervisor d-space pdr %u" }, //
				{ 0172240, 8, 2, "supervisor i-space par %u" }, //
				{ 0172260, 8, 2, "supervisor d-space par %u" }, //
				{ 0172300, 8, 2, "kernel i-space pdr %u" }, //
				{ 0172320, 8, 2, "kernel d-space pdr %u" }, //
				{ 0172340, 8, 2, "kernel i-space par %u" }, //
				{ 0172360, 8, 2, "kernel d-space par %u" }, //
				{ 0172410, 1, 2, "dr11-b csr" }, //
				{ 0172440, 1, 2, "rh11/rh70 #2 rpcs1 (control/status 1)" }, //
				{ 0172516, 1, 2, "mmr3 (memory management register 3)" }, //
				{ 0172520, 1, 2, "tm11 mts (status)" }, //
				{ 0172522, 1, 2, "tm11 mtc (command)" }, //
				{ 0172524, 1, 2, "tm11 mtbrc (byte record counter)" }, //
				{ 0172526, 1, 2, "tm11 mtcma (current memory address)" }, //
				{ 0172530, 1, 2, "tm11 mtd (data buffer)" }, //
				{ 0172532, 1, 2, "tm11 mtrd (tu10 read lines)" }, //
				{ 0172540, 1, 2, "kw11-p csr (programmable clock)" }, //
				{ 0172542, 1, 2, "kw11-p count set buffer" }, //
				{ 0172544, 1, 2, "kw11-p counter" }, //
				{ 0174400, 1, 2, "rl11 rlcs (control/status)" }, //
				{ 0174402, 1, 2, "rl11 rlba (bus address)" }, //
				{ 0174404, 1, 2, "rl11 rlda (disk address)" }, //
				{ 0174406, 1, 2, "rl11 rlmp (multipurpose)" }, //
				{ 0174410, 1, 2, "rlv12 rlbae (bus address extension)" }, //
				{ 0174440, 1, 2, "deqna/delqa csr" }, //
				{ 0174500, 1, 2, "tqk50 tmscp ip" }, //
				{ 0174510, 1, 2, "deuna csr" }, //
				{ 0176700, 1, 2, "rh11/rh70 rpcs1 (control/status 1)" }, //
				{ 0176702, 1, 2, "rh11/rh70 rpwc (word count)" }, //
				{ 0176704, 1, 2, "rh11/rh70 rpba (bus address)" }, //
				{ 0176706, 1, 2, "rh11/rh70 rpda (disk address)" }, //
				{ 0176710, 1, 2, "rh11/rh70 rpcs2 (control/status 2) / rp11 csr" }, //
				{ 0176712, 1, 2, "rh11/rh70 rpds (drive status)" }, //
				{ 0176714, 1, 2, "rh11/rh70 rper1 (error 1)" }, //
				{ 0177160, 1, 2, "cr11 card reader csr" }, //
				{ 0177170, 1, 2, "rx11/rx211 rxcs (command and status)" }, //
				{ 0177172, 1, 2, "rx11/rx211 rxdb (data buffer)" }, //
				{ 0177300, 1, 2, "ke11-a div (divide)" }, //
				{ 0177302, 1, 2, "ke11-a ac (accumulator)" }, //
				{ 0177304, 1, 2, "ke11-a mq (multiplier quotient)" }, //
				{ 0177306, 1, 2, "ke11-a mul (multiply)" }, //
				{ 0177310, 1, 2, "ke11-a sc/sr (step count / status)" }, //
				{ 0177312, 1, 2, "ke11-a nor (normalize)" }, //
				{ 0177314, 1, 2, "ke11-a lsh (logical shift)" }, //
				{ 0177316, 1, 2, "ke11-a ash (arithmetic shift)" }, //
				{ 0177340, 1, 2, "tc11 tcst (status)" }, //
				{ 0177342, 1, 2, "tc11 tccm (command)" }, //
				{ 0177344, 1, 2, "tc11 tcwc (word count)" }, //
				{ 0177346, 1, 2, "tc11 tcba (bus address)" }, //
				{ 0177350, 1, 2, "tc11 tcdt (data)" }, //
				{ 0177400, 1, 2, "rk11 rkds (drive status)" }, //
				{ 0177402, 1, 2, "rk11 rker (error)" }, //
				{ 0177404, 1, 2, "rk11 rkcs (control/status)" }, //
				{ 0177406, 1, 2, "rk11 rkwc (word count)" }, //
				{ 0177410, 1, 2, "rk11 rkba (bus address)" }, //
				{ 0177412, 1, 2, "rk11 rkda (disk address)" }, //
				{ 0177416, 1, 2, "rk11 rkdb (data buffer)" }, //
				{ 0177440, 1, 2, "rc11 / rk611 csr" }, //
				{ 0177460, 1, 2, "rf11 dcs (control/status)" }, //
				{ 0177462, 1, 2, "rf11 wc (word count)" }, //
				{ 0177464, 1, 2, "rf11 cma (current memory address)" }, //
				{ 0177466, 1, 2, "rf11 dar (disk address)" }, //
				{ 0177470, 1, 2, "rf11 dae (disk address extension)" }, //
				{ 0177472, 1, 2, "rf11 dbr (data buffer)" }, //
				{ 0177474, 1, 2, "rf11 ma (maintenance)" }, //
				{ 0177476, 1, 2, "rf11 ads (address of disk segment)" }, //
				{ 0177500, 1, 2, "ta11 cassette csr / qbus doorbell" }, //
				{ 0177514, 1, 2, "lp11 lps (status)" }, //
				{ 0177516, 1, 2, "lp11 lpb (data buffer)" }, //
				{ 0177546, 1, 2, "kw11-l lks (line clock status)" }, //
				{ 0177550, 1, 2, "pc11 prs (reader status)" }, //
				{ 0177552, 1, 2, "pc11 prb (reader buffer)" }, //
				{ 0177554, 1, 2, "pc11 pps (punch status)" }, //
				{ 0177556, 1, 2, "pc11 ppb (punch buffer)" }, //
				{ 0177560, 1, 2, "console dl11 rcsr (receiver status)" }, //
				{ 0177562, 1, 2, "console dl11 rbuf (receiver buffer)" }, //
				{ 0177564, 1, 2, "console dl11 xcsr (transmitter status)" }, //
				{ 0177566, 1, 2, "console dl11 xbuf (transmitter buffer)" }, //
				{ 0177570, 1, 2, "console switch register / display register" }, //
				{ 0177572, 1, 2, "mmr0 (memory management register 0)" }, //
				{ 0177574, 1, 2, "mmr1 (instruction length/register change)" }, //
				{ 0177576, 1, 2, "mmr2 (virtual address of the instruction)" }, //
				{ 0177600, 8, 2, "user i-space pdr %u" }, //
				{ 0177620, 8, 2, "user d-space pdr %u" }, //
				{ 0177640, 8, 2, "user i-space par %u" }, //
				{ 0177660, 8, 2, "user d-space par %u" }, //
				// the general registers, one *byte* address each (11/45, 11/70)
				{ 0177700, 6, 1, "register r%u, set 0" }, //
				{ 0177706, 1, 1, "kernel stack pointer" }, //
				{ 0177707, 1, 1, "pc" }, //
				{ 0177710, 6, 1, "register r%u, set 1" }, //
				{ 0177716, 1, 1, "supervisor stack pointer" }, //
				{ 0177717, 1, 1, "user stack pointer" }, //
				{ 0177740, 1, 2, "low error address register" }, //
				{ 0177742, 1, 2, "high error address register" }, //
				{ 0177744, 1, 2, "memory system error register" }, //
				{ 0177746, 1, 2, "cache control register" }, //
				{ 0177750, 1, 2, "maintenance register" }, //
				{ 0177752, 1, 2, "hit/miss register" }, //
				{ 0177760, 1, 2, "lower size register" }, //
				{ 0177762, 1, 2, "upper size register" }, //
				{ 0177764, 1, 2, "system i/d register" }, //
				{ 0177766, 1, 2, "cpu error register" }, //
				{ 0177770, 1, 2, "microprogram break register" }, //
				{ 0177772, 1, 2, "pirq (program interrupt request)" }, //
				{ 0177774, 1, 2, "stack limit register" }, //
				{ 0177776, 1, 2, "psw (processor status word)" }, //
				{ 0, 0, 0, NULL } };

// <descrs> entry covering <addr>, with the index inside its block, else NULL
static const addrdescr_t *find_addrdescr(const addrdescr_t *descrs, uint16_t addr, unsigned *index)
{
	for (unsigned i = 0; descrs[i].name; i++) {
		if (addr < descrs[i].startaddr)
			continue;
		unsigned offset = addr - descrs[i].startaddr;
		if (offset % descrs[i].step)
			continue;
		unsigned idx = offset / descrs[i].step;
		if (idx < descrs[i].count) {
			*index = idx;
			return &descrs[i];
		}
	}
	return NULL;
}

static std::string addrdescr_text(const addrdescr_t *descr, unsigned index)
{
	std::string name(descr->name);
	size_t pos = name.find("%u");
	if (pos == std::string::npos)
		return name;
	char buffer[16];
	sprintf(buffer, "%u", index);
	return name.substr(0, pos) + buffer + name.substr(pos + 2);
}

std::string pdp11disas_address_info(uint32_t addr)
{
	uint16_t a = (uint16_t) addr;
	unsigned index = 0;
	const addrdescr_t *descr;

	// The vectors: two words each, the new PC and the new PSW.
	if (a < 01000) {
		descr = find_addrdescr(vectordescrs, (uint16_t) (a & ~3), &index);
		if (descr == NULL)
			return std::string();
		std::string result = addrdescr_text(descr, index);
		if (a & 2)
			result += ", new psw";
		if (a & 1)
			result += ", high byte";
		return result;
	}

	descr = find_addrdescr(iopagedescrs, a, &index);
	if (descr)
		return addrdescr_text(descr, index);
	// a byte access to the high half of a register
	if (a & 1) {
		descr = find_addrdescr(iopagedescrs, (uint16_t) (a - 1), &index);
		if (descr && descr->step > 1)
			return addrdescr_text(descr, index) + ", high byte";
	}
	return std::string();
}

/**********************************************************************
 * formatting helpers
 **********************************************************************/

// zero padded octal, <digits> wide. digits == 0: minimal width
static std::string octal(uint32_t value, unsigned digits)
{
	char buffer[32];
	if (digits)
		sprintf(buffer, "%0*o", (int) digits, value);
	else
		sprintf(buffer, "%o", value);
	return std::string(buffer);
}

// addresses, absolute/relative targets, literal words: always 6 octal digits,
// 8 for the bus addresses above the 16 bit range
static std::string octal_addr(uint32_t addr)
{
	return octal(addr, addr > 0777777 ? 8 : 6);
}

static const char *regnames[8] = { "r0", "r1", "r2", "r3", "r4", "r5", "sp", "pc" };

static std::string regname(unsigned r)
{
	return std::string(regnames[r & 7]);
}

// FP11 accumulator. The "ac" field of an instruction is 2 bits wide, but a
// floating operand in register mode may name ac0..ac7 (6 and 7 do not exist).
static std::string acname(unsigned f)
{
	return "ac" + octal(f & 7, 0);
}

/**********************************************************************
 * the disassembler
 **********************************************************************/

// decoding state of one instruction
class decoder_c {
public:
	pdp11disas_memory_c *memory;
	pdp11disas_instruction_c *instr;
	uint32_t cursor;	// address of the next word to fetch
	bool ok;		// false: a needed word is not readable

	// Fetch the next word of the instruction into the instruction image.
	// false: not readable, or the instruction would get too long.
	bool fetch(uint16_t *w);
	// One 6 bit "mode,register" operand. <integer_reg> false: register mode
	// names an FP11 accumulator, not a general register.
	std::string operand(unsigned spec, bool integer_reg);
	// Remember <addr> for the listing comment if it is a well known one.
	void note_address(uint16_t addr);
};

void decoder_c::note_address(uint16_t addr)
{
	if (pdp11disas_address_info(addr).empty())
		return;
	// both operands may name the same register: say it once
	for (unsigned i = 0; i < instr->known_addresses.size(); i++)
		if (instr->known_addresses[i] == addr)
			return;
	instr->known_addresses.push_back(addr);
}

bool decoder_c::fetch(uint16_t *w)
{
	if (!ok)
		return false;
	if (instr->wordcount >= PDP11DISAS_MAX_WORDS || !memory->get_word(cursor, w)) {
		ok = false;
		return false;
	}
	instr->word[instr->wordcount++] = *w;
	cursor += 2;
	return true;
}

std::string decoder_c::operand(unsigned spec, bool integer_reg)
{
	unsigned reg = spec & 7;
	unsigned mode = (spec >> 3) & 7;
	uint16_t nval;

	if (!ok)
		return std::string();

	switch (mode) {
	case 0:	// register
		return integer_reg ? regname(reg) : acname(reg);
	case 1:	// register deferred
		return "(" + regname(reg) + ")";
	case 2:	// autoincrement, with pc: immediate
		if (reg != 7)
			return "(" + regname(reg) + ")+";
		if (!fetch(&nval))
			return std::string();
		// An immediate is a number, not an address - "mov #4,r0" loads a 4,
		// it does not mean the bus error vector. Only a value in the I/O page
		// is annotated: there it is a device address being loaded into a
		// register, which is how nearly all device drivers start.
		if (nval >= 0160000)
			note_address(nval);
		return "#" + octal(nval, 6);
	case 3:	// autoincrement deferred, with pc: absolute
		if (reg != 7)
			return "@(" + regname(reg) + ")+";
		if (!fetch(&nval))
			return std::string();
		note_address(nval);
		return "@#" + octal(nval, 6);
	case 4:	// autodecrement
		return "-(" + regname(reg) + ")";
	case 5:	// autodecrement deferred
		return "@-(" + regname(reg) + ")";
	case 6:	// index, with pc: relative
		if (!fetch(&nval))
			return std::string();
		if (reg != 7)
			return octal(nval, 6) + "(" + regname(reg) + ")";
		// cursor is now the PC the CPU would add: the word after this one
		note_address((uint16_t) (nval + cursor));
		return octal((uint16_t) (nval + cursor), 6);
	case 7:	// index deferred, with pc: relative deferred
		if (!fetch(&nval))
			return std::string();
		if (reg != 7)
			return "@" + octal(nval, 6) + "(" + regname(reg) + ")";
		note_address((uint16_t) (nval + cursor));
		return "@" + octal((uint16_t) (nval + cursor), 6);
	}
	return std::string();	// not reachable, mode is 3 bits
}

uint32_t pdp11disas_instruction(const pdp11disas_options_c& options, pdp11disas_memory_c& memory,
		uint32_t addr, pdp11disas_instruction_c *result)
{
	decoder_c decoder;
	uint16_t ir;
	unsigned idx;
	std::string operands, s1, s2;

	*result = pdp11disas_instruction_c();
	result->addr = addr;
	result->cpu_model = options.cpu_model;

	decoder.memory = &memory;
	decoder.instr = result;
	decoder.cursor = addr;
	decoder.ok = true;

	if (!decoder.fetch(&ir)) {
		// not even the opcode is readable: the caller must stop here
		result->truncated = true;
		return addr + 2;
	}

	// find the instruction. The table is ordered, the first match wins.
	for (idx = 0; idx < OPTABLE_COUNT; idx++)
		if ((ir & optable[idx].mask) == optable[idx].opval)
			break;
	if (idx >= OPTABLE_COUNT) {
		// no instruction on any PDP-11
		result->mnemonic = ".word";
		result->operands = octal(ir, 6);
		return addr + 2;
	}

	const opentry_t *entry = &optable[idx];
	unsigned namevariant = 0;
	if (options.options & pdp11disas_opt_fpd)
		namevariant |= 2;
	if (options.options & pdp11disas_opt_fpl)
		namevariant |= 1;
	if (entry->name[namevariant] == NULL)
		namevariant = 0;

	unsigned srcspec = (ir >> 6) & 077;	// "SS" field
	unsigned dstspec = ir & 077;		// "DD" field
	unsigned reg3 = srcspec & 7;		// 3 bit register field
	unsigned fac2 = (ir >> 6) & 3;		// 2 bit FP11 accumulator field
	unsigned lit8 = ir & 0377;

	switch (entry->cls) {
	case cls_npn:
		break;
	case cls_reg:
		operands = regname(dstspec & 7);
		break;
	case cls_3b:	// spl: the level is in bits 2..0
		operands = octal(ir & 7, 0);
		break;
	case cls_6b:
		operands = octal(dstspec, 0);
		break;
	case cls_8b:
		operands = octal(lit8, 0);
		break;
	case cls_br:
		operands = octal((uint16_t) (addr + 2 + 2 * (int) (int8_t) lit8), 6);
		break;
	case cls_sob:
		operands = regname(reg3) + "," + octal((uint16_t) (addr + 2 - 2 * dstspec), 6);
		break;
	case cls_sop:
		operands = decoder.operand(dstspec, true);
		break;
	case cls_fop:
		operands = decoder.operand(dstspec, false);
		break;
	case cls_dop:
		s1 = decoder.operand(srcspec, true);
		s2 = decoder.operand(dstspec, true);
		operands = s1 + "," + s2;
		break;
	case cls_rsop:	// jsr rn,dst / xor rn,dst
		s2 = decoder.operand(dstspec, true);
		operands = regname(reg3) + "," + s2;
		break;
	case cls_sopr:	// mul src,rn
		s1 = decoder.operand(dstspec, true);
		operands = s1 + "," + regname(reg3);
		break;
	case cls_afop:	// stf acn,fltdst
		s2 = decoder.operand(dstspec, false);
		operands = acname(fac2) + "," + s2;
		break;
	case cls_fopa:	// addf fltsrc,acn
		s1 = decoder.operand(dstspec, false);
		operands = s1 + "," + acname(fac2);
		break;
	case cls_asop:	// stexp acn,intdst
		s2 = decoder.operand(dstspec, true);
		operands = acname(fac2) + "," + s2;
		break;
	case cls_sopa:	// ldexp intsrc,acn
		s1 = decoder.operand(dstspec, true);
		operands = s1 + "," + acname(fac2);
		break;
	}

	if (!decoder.ok) {
		// an operand word is missing: only the opcode word is certain, and
		// what the half decoded operands named means nothing
		result->mnemonic = ".word";
		result->operands = octal(ir, 6);
		result->truncated = true;
		result->wordcount = 1;
		result->known_addresses.clear();
		return addr + 2;
	}

	result->mnemonic = entry->name[namevariant];
	result->operands = operands;
	result->required_options = entry->required;
	result->known = true;
	result->available = options.has(entry->required);
	return addr + 2 * result->wordcount;
}

uint32_t pdp11disas_region(const pdp11disas_options_c& options, pdp11disas_memory_c& memory,
		uint32_t addr, unsigned instruction_count, std::vector<pdp11disas_instruction_c> *result)
{
	pdp11disas_instruction_c instr;

	for (unsigned i = 0; i < instruction_count; i++) {
		uint32_t next = pdp11disas_instruction(options, memory, addr, &instr);
		if (instr.wordcount == 0)
			break;	// memory not readable: stay on the address which failed
		addr = next;
		result->push_back(instr);
	}
	return addr;
}

/**********************************************************************
 * output of a single instruction
 **********************************************************************/

std::string pdp11disas_instruction_c::comment(void) const
{
	std::string result;

	if (wordcount == 0)
		return "memory not readable";
	// what is wrong with the instruction comes first, then what its operands
	// point at
	if (truncated)
		result = "instruction incomplete, memory not readable";
	else if (known && !available)
		result = pdp11disas_options_c::options_as_text(required_options) + " not on pdp-"
				+ cpu_model;

	for (unsigned i = 0; i < known_addresses.size(); i++) {
		if (!result.empty())
			result += ", ";
		result += octal(known_addresses[i], 6) + " = "
				+ pdp11disas_address_info(known_addresses[i]);
	}
	return result;
}

std::string pdp11disas_instruction_c::text(void) const
{
	std::string result;
	char buffer[64];

	if (operands.empty())
		result = mnemonic;
	else {
		sprintf(buffer, "%-8s", mnemonic.substr(0, 32).c_str());
		result = std::string(buffer) + operands;
	}
	std::string note = comment();
	if (!note.empty()) {
		// pad the instruction out so the comments line up. An instruction
		// wider than the column still gets its separating space.
		if (result.length() >= 24)
			result += " ";
		else
			while (result.length() < 24)
				result += " ";
		result += "; " + note;
	}
	return result;
}

std::string pdp11disas_instruction_c::listing_line(void) const
{
	std::string result = octal_addr(addr) + "  ";

	// the words of the instruction, in fixed columns
	for (unsigned i = 0; i < PDP11DISAS_MAX_WORDS; i++)
		if (i < wordcount)
			result += octal(word[i], 6) + " ";
		else
			result += "       ";
	result += " " + text();
	return result;
}

/**********************************************************************
 * pdp11disas_buffermemory_c
 **********************************************************************/

pdp11disas_buffermemory_c::pdp11disas_buffermemory_c(const uint16_t *_words, unsigned _wordcount,
		uint32_t _startaddr)
{
	words = _words;
	wordcount = _wordcount;
	startaddr = _startaddr;
}

bool pdp11disas_buffermemory_c::get_word(uint32_t addr, uint16_t *word)
{
	if (addr < startaddr || (addr & 1))
		return false;
	uint32_t idx = (addr - startaddr) / 2;
	if (idx >= wordcount)
		return false;
	*word = words[idx];
	return true;
}
