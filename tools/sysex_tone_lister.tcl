#!/usr/bin/env tclsh
# sysex_tone_lister.tcl - list the sound records found in a SysEx dump: how many, their
# names, and (on request) their full synthesis parameters. Primarily a Roland D-110 tool, but
# also reads Yamaha DX7 32-voice bulk dumps (Alan's request, 2026-09-01 - he has both).
#
# Independent of any plugin's own C++/build system - reads raw bytes only, no JUCE/CMake/Tcl
# extensions needed beyond the core language.
#
# D-110: same DT1 message shape and address range
# ~/src/D110/d110-vst-emulator/plugin/Source/SoundbankDatabase.cpp's own
# decodeTonesFromMessage() decodes (SysEx 08 00 00, 256 bytes/tone: 10-byte name + 246-byte
# body). The per-partial parameter offsets are copied from that project's
# plugin/Source/PluginEditor.cpp's kWg/kPitchEnv/kTvf/kTva tables, which are themselves
# transcribed from Roland's own "Tone Parameters" reference card - not reverse-engineered
# here, just carried over.
#
# DX7: the 32-voice "packed" bulk dump (SysEx F0 43 0n 09 20 00 <4096 bytes> <checksum> F7) -
# the format actual patch banks are distributed in (a single-voice edit-buffer dump, format 0,
# is a different, unpacked 155-byte layout and is NOT decoded here). The 128-byte-per-voice
# packed layout and its checksum (two's complement of the byte sum, mod 128 - same style
# Roland uses) were verified empirically against a real bank (/tmp/SynprezFM_01.syx) before
# writing this: computed checksum matched the file's own, and decoded name/algorithm/feedback/
# transpose bytes all came out as plausible values (printable 10-char names, algorithm 0-31,
# transpose centered on 24) - not just copied from a spec sheet unverified.
#
# Usage:
#   tclsh sysex_tone_lister.tcl <file.syx>              List every sound: index, name
#   tclsh sysex_tone_lister.tcl <file.syx> all           Also dump full parameters for every sound
#   tclsh sysex_tone_lister.tcl <file.syx> <index>       Dump full parameters for sound #<index> (1-based)
#   tclsh sysex_tone_lister.tcl <file.syx> <text>        Dump parameters for every sound whose name
#                                                         contains <text> (case-insensitive)
# The selector (index/text/all) applies to whichever kind of sound the file actually contains -
# D-110 tones and DX7 voices are listed/numbered separately if a file somehow had both.
#
# Only plain concatenated SysEx bytes are scanned (a real .syx bulk dump); a standard MIDI
# file's SysEx meta-events happen to be readable the same way too since each one still starts
# with 0xF0 and ends with 0xF7, but delta-time bytes ahead of an event are not stripped first,
# so a .mid/.smf file is best converted to raw .syx before running this if it looks off.

# bail out immediately when invoked by bash tab-completion (same fix as writhdeck's boot
# scripts, ~/src/writerdeck/writhdeck/writhdeck.tcl - bash's own default completion runs an
# executable with COMP_LINE/COMP_POINT set to ask it for completions; a script that doesn't
# expect that just tries to open whatever partial path is on the command line so far, which
# breaks the terminal mid-completion instead of quietly doing nothing)
if {[info exists ::env(COMP_LINE)] || [info exists ::env(COMP_POINT)]} { exit 0 }

set kToneRecordSize 256
set kNumToneSlots 64
set kToneAreaBase [expr {0x08 << 14}] ;# 7-bit-packed form of SysEx address 08 00 00

# {name  offset(record byte 10 = offset 0)  hi} - the 4 bytes shared by all partials.
set commonFields {
	{STRUCTURE-1-2   0  12}
	{STRUCTURE-3-4   1  12}
	{PARTIAL-MUTE    2  15}
	{ENV-MODE        3   1}
}

# {name  offset(within THIS partial's own 58-byte block)  hi} - WG/Pitch-Env, TVF, TVA, in
# the order Roland's own card lists them. Value shown is the raw stored byte (0..hi), same as
# the extended editor's TONE tab shows it - no attempt here to reinterpret bias points, key
# scaling etc. into musical units.
set partialFields {
	{WG-PITCH-COARSE    0   96}
	{WG-PITCH-FINE      1  100}
	{WG-PITCH-KF        2   16}
	{WG-BENDER-SW       3    1}
	{WG-WAVEFORM        4    3}
	{PCM-WAVE           5  127}
	{WG-PULSE-WIDTH     6  100}
	{WG-PW-VELO         7   14}
	{P-ENV-DEPTH        8   10}
	{P-ENV-VELO         9  100}
	{P-ENV-TIME-KF     10    4}
	{P-ENV-T1          11  100}
	{P-ENV-T2          12  100}
	{P-ENV-T3          13  100}
	{P-ENV-T4          14  100}
	{P-ENV-L0          15  100}
	{P-ENV-L1          16  100}
	{P-ENV-L2          17  100}
	{P-ENV-SUS-L       18  100}
	{P-ENV-END-L       19  100}
	{P-LFO-RATE        20  100}
	{P-LFO-DEPTH       21  100}
	{P-LFO-MOD         22  100}
	{TVF-FREQ          23  100}
	{TVF-RESO          24   30}
	{TVF-FREQ-KF       25   14}
	{TVF-BIAS-P        26  127}
	{TVF-BIAS-LVL      27   14}
	{TVF-ENV-DEPTH     28  100}
	{TVF-ENV-VELO      29  100}
	{TVF-ENV-DKF       30    4}
	{TVF-ENV-TKF       31    4}
	{TVF-ENV-T1        32  100}
	{TVF-ENV-T2        33  100}
	{TVF-ENV-T3        34  100}
	{TVF-ENV-T4        35  100}
	{TVF-ENV-T5        36  100}
	{TVF-ENV-L1        37  100}
	{TVF-ENV-L2        38  100}
	{TVF-ENV-L3        39  100}
	{TVF-ENV-SUS-L     40  100}
	{TVA-LEVEL         41  100}
	{TVA-VELOCITY      42  100}
	{TVA-BIAS-P1       43  127}
	{TVA-BIAS-L1       44   12}
	{TVA-BIAS-P2       45  127}
	{TVA-BIAS-L2       46   12}
	{TVA-ENV-TKF       47    4}
	{TVA-ENV-T1VF      48    4}
	{TVA-ENV-T1        49  100}
	{TVA-ENV-T2        50  100}
	{TVA-ENV-T3        51  100}
	{TVA-ENV-T4        52  100}
	{TVA-ENV-T5        53  100}
	{TVA-ENV-L1        54  100}
	{TVA-ENV-L2        55  100}
	{TVA-ENV-L3        56  100}
	{TVA-ENV-SUS-L     57  100}
}

# {name  offset(within THIS operator's own 17-byte block)  hi} - the plain byte-aligned fields
# only; the bit-packed ones (scale curves, rate-scale+detune, ams+kvs, mode+coarse) are pulled
# apart by hand in dumpDX7VoiceParams below instead of being forced into this table's shape.
set dx7OpFields {
	{EG-R1                0   99}
	{EG-R2                1   99}
	{EG-R3                2   99}
	{EG-R4                3   99}
	{EG-L1                4   99}
	{EG-L2                5   99}
	{EG-L3                6   99}
	{EG-L4                7   99}
	{LEVEL-SCALE-BP       8   99}
	{SCALE-LEFT-DEPTH     9   99}
	{SCALE-RIGHT-DEPTH   10   99}
}

proc readFileBytes {path} {
	set f [open $path rb]
	fconfigure $f -translation binary -encoding binary
	set data [read $f]
	close $f
	return $data
}

# Every concatenated F0..F7 span in the file, each as a list of unsigned byte values.
proc extractSysexMessages {data} {
	binary scan $data cu* bytes
	set len [llength $bytes]
	set messages {}
	set i 0
	while {$i < $len} {
		if {[lindex $bytes $i] == 0xF0} {
			set j [expr {$i + 1}]
			while {$j < $len && [lindex $bytes $j] != 0xF7} { incr j }
			if {$j < $len} {
				lappend messages [lrange $bytes $i $j]
				set i [expr {$j + 1}]
				continue
			}
			break ;# unterminated trailing message - ignore it
		}
		incr i
	}
	return $messages
}

# Decodes one message into zero or more {slot recordBytes} pairs - mirrors
# SoundbankDatabase.cpp's decodeTonesFromMessage() exactly (Roland DT1 write, addressed
# inside the internal Tone area, checksum verified; a message can carry more than one
# consecutive 256-byte record, a real "dump the whole bank" capture does).
proc decodeToneMessage {msg} {
	global kToneRecordSize kToneAreaBase kNumToneSlots
	set result {}
	set len [llength $msg]
	if {$len < 11} { return $result }
	if {[lindex $msg 0] != 0xF0 || [lindex $msg end] != 0xF7} { return $result }
	if {[lindex $msg 1] != 0x41} { return $result } ;# Roland
	if {[lindex $msg 3] != 0x16} { return $result } ;# model: MT-32 family, the D-110 answers to
	if {[lindex $msg 4] != 0x12} { return $result } ;# DT1

	set dataLen [expr {$len - 10}]
	if {$dataLen < $kToneRecordSize} { return $result }

	set a1 [expr {[lindex $msg 5] & 0x7f}]
	set a2 [expr {[lindex $msg 6] & 0x7f}]
	set a3 [expr {[lindex $msg 7] & 0x7f}]
	set target [expr {($a1 << 14) | ($a2 << 7) | $a3}]
	if {$target < $kToneAreaBase} { return $result }
	set offset [expr {$target - $kToneAreaBase}]
	if {($offset % $kToneRecordSize) != 0 || $offset >= ($kNumToneSlots * $kToneRecordSize)} {
		return $result
	}

	set sum [expr {$a1 + $a2 + $a3}]
	for {set k 0} {$k < $dataLen} {incr k} {
		incr sum [expr {[lindex $msg [expr {8 + $k}]] & 0x7f}]
	}
	set expected [expr {(128 - ($sum & 0x7f)) & 0x7f}]
	set actual [expr {[lindex $msg [expr {8 + $dataLen}]] & 0x7f}]
	if {$actual != $expected} { return $result }

	set i 0
	while {(($i + 1) * $kToneRecordSize) <= $dataLen
	       && ($offset + $i * $kToneRecordSize + $kToneRecordSize) <= ($kNumToneSlots * $kToneRecordSize)} {
		set start [expr {8 + $i * $kToneRecordSize}]
		set rec [lrange $msg $start [expr {$start + $kToneRecordSize - 1}]]
		set slot [expr {($offset / $kToneRecordSize) + $i}]
		lappend result [list $slot $rec]
		incr i
	}
	return $result
}

# Decodes a Yamaha DX7 32-voice PACKED bulk dump (SysEx F0 43 0n 09 20 00 <4096 bytes>
# <checksum> F7 - format byte 9, byte count 4096) into 32 raw 128-byte voice records. Returns
# {} for anything else, including a single-voice edit-buffer dump (format 0, a different
# 155-byte UNPACKED layout this tool doesn't decode).
proc decodeDX7Message {msg} {
	set result {}
	set len [llength $msg]
	if {$len < 6} { return $result }
	if {[lindex $msg 0] != 0xF0 || [lindex $msg end] != 0xF7} { return $result }
	if {[lindex $msg 1] != 0x43} { return $result } ;# Yamaha
	if {[lindex $msg 3] != 9} { return $result }    ;# format 9 = 32-voice bulk

	set countMsb [lindex $msg 4]
	set countLsb [lindex $msg 5]
	set byteCount [expr {($countMsb << 7) | $countLsb}]
	if {$byteCount != 4096} { return $result }
	if {$len < 6 + 4096 + 2} { return $result } ;# header + data + checksum + F7

	# Two's complement of the sum of the 4096 data bytes, mod 128 - verified against a real
	# bank file, see this script's own header comment.
	set sum 0
	for {set k 0} {$k < 4096} {incr k} {
		incr sum [lindex $msg [expr {6 + $k}]]
	}
	set expected [expr {(128 - ($sum & 0x7f)) & 0x7f}]
	set actual [lindex $msg [expr {6 + 4096}]]
	if {$actual != $expected} { return $result }

	for {set v 0} {$v < 32} {incr v} {
		set start [expr {6 + $v * 128}]
		lappend result [lrange $msg $start [expr {$start + 127}]]
	}
	return $result
}

# DX7 voice names are always all 10 bytes (space-padded, not null/short like a D-110 name can
# be) - replace anything not printable ASCII with '?' instead of truncating there, so a
# genuinely corrupt name is visible rather than silently shortened.
proc dx7TrimName {rec} {
	set name ""
	for {set c 118} {$c < 128} {incr c} {
		set ch [lindex $rec $c]
		if {$ch < 32 || $ch > 126} { set ch 0x3F } ;# '?'
		append name [format %c $ch]
	}
	return [string trimright $name]
}

# Roland D-50/D-550 (model 0x14) - Alan's request, 2026-09-01. Verified against the official
# Roland D-50 MIDI Implementation manual (Alan supplied /tmp/d50/D-50MidiImplementation.pdf)
# AND against two real bank dumps (/tmp/FactoryPatches.syx, /tmp/Marc-J.syx): both decode to
# entirely plausible real patch/tone names this way (e.g. "Ancient Dreams", "ABACAB solo",
# "JX-Voices") - not just transcribed from the manual unverified.
#
# D-50 names use a CUSTOM 64-character set (index 0-63), not raw ASCII - manual section *4-5:
# value 0-63 = ' ','A'-'Z','a'-'z','1'-'9','0','-', in that order. A naive raw-ASCII read (like
# the D-110/DX7 name fields use) produces garbage on real D-50 files; this was the key
# unblocking discovery here.
set kD50Charset " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz1234567890-"

proc d50Name {rec start len} {
	global kD50Charset
	set clen [string length $kD50Charset]
	set s ""
	for {set i 0} {$i < $len} {incr i} {
		set v [lindex $rec [expr {$start + $i}]]
		if {$v >= 0 && $v < $clen} {
			append s [string index $kD50Charset $v]
		} else {
			append s "?"
		}
	}
	return [string trimright $s]
}

# {name  offset(within a 64-byte partial block)  hi} - manual section *4-4, both Upper and
# Lower share this layout (WG=waveform generator/pitch, TVF=filter, TVA=amplitude - the D-50's
# own equivalent of the D-110's kWg/kPitchEnv/kTvf/kTva tables). Offsets 54-63 are Roland's own
# "Extension (for future)" padding, included for completeness even though nothing is known to
# use them.
set d50PartialFields {
	{WG-PITCH-COARSE            0   72}
	{WG-PITCH-FINE              1  100}
	{WG-PITCH-KEYFOLLOW         2   16}
	{WG-LFO-MODE                3    3}
	{WG-P-ENV-MODE              4    2}
	{WG-BEND-MODE               5    2}
	{WG-WAVE-FORM               6    1}
	{WG-PCM-WAVE-NO             7   99}
	{WG-PULSE-WIDTH             8  100}
	{WG-PW-VELOCITY-RANGE       9   14}
	{WG-PW-LFO-SELECT          10    5}
	{WG-PW-LFO-DEPTH           11  100}
	{WG-PW-AFTERTOUCH-RANGE    12   14}
	{TVF-CUTOFF-FREQUENCY      13  100}
	{TVF-RESONANCE             14   30}
	{TVF-KEYFOLLOW             15   14}
	{TVF-BIAS-POINT            16  127}
	{TVF-BIAS-LEVEL            17   14}
	{TVF-ENV-DEPTH             18  100}
	{TVF-ENV-VELOCITY-RANGE    19  100}
	{TVF-ENV-DEPTH-KEYFOLLOW   20    4}
	{TVF-ENV-TIME-KEYFOLLOW    21    4}
	{TVF-ENV-TIME-1            22  100}
	{TVF-ENV-TIME-2            23  100}
	{TVF-ENV-TIME-3            24  100}
	{TVF-ENV-TIME-4            25  100}
	{TVF-ENV-TIME-5            26  100}
	{TVF-ENV-LEVEL-1           27  100}
	{TVF-ENV-LEVEL-2           28  100}
	{TVF-ENV-LEVEL-3           29  100}
	{TVF-ENV-SUSTAIN-LEVEL     30  100}
	{TVF-ENV-END-LEVEL         31    1}
	{TVF-MOD-LFO-SELECT        32    5}
	{TVF-MOD-LFO-DEPTH         33  100}
	{TVF-MOD-AFTERTOUCH-RANGE  34   14}
	{TVA-LEVEL                 35  100}
	{TVA-VELOCITY-RANGE        36  100}
	{TVA-BIAS-POINT            37  127}
	{TVA-BIAS-LEVEL            38   12}
	{TVA-ENV-TIME-1            39  100}
	{TVA-ENV-TIME-2            40  100}
	{TVA-ENV-TIME-3            41  100}
	{TVA-ENV-TIME-4            42  100}
	{TVA-ENV-TIME-5            43  100}
	{TVA-ENV-LEVEL-1           44  100}
	{TVA-ENV-LEVEL-2           45  100}
	{TVA-ENV-LEVEL-3           46  100}
	{TVA-ENV-SUSTAIN-LEVEL     47  100}
	{TVA-ENV-END-LEVEL         48    1}
	{TVA-ENV-VELOCITY-FOLLOW   49    4}
	{TVA-ENV-TIME-KEYFOLLOW    50    4}
	{TVA-MOD-LFO-SELECT        51    5}
	{TVA-MOD-LFO-DEPTH         52  100}
	{TVA-MOD-AFTERTOUCH-RANGE  53   14}
	{EXTENSION-54               54  127}
	{EXTENSION-55               55  127}
	{EXTENSION-56               56  127}
	{EXTENSION-57               57  127}
	{EXTENSION-58               58  127}
	{EXTENSION-59               59  127}
	{EXTENSION-60               60  127}
	{EXTENSION-61               61  127}
	{EXTENSION-62               62  127}
	{EXTENSION-63               63  127}
}

# {name  offset(within a 64-byte common block)  hi} - manual section *4-5, minus offsets 0-9
# (Tone Name, handled separately by d50Name). Offset 39 is printed "Low EQ Frequency" a second
# time in the manual's own OCR-prone table right after "Low EQ Gain" at 38, immediately before
# "High EQ Q"/"High EQ Gain" at 40/41 - almost certainly a "High EQ Frequency" the scan
# rendered wrong, so labelled that way here rather than repeated verbatim.
set d50CommonFields {
	{STRUCTURE-NO                10    6}
	{P-ENV-VELOCITY-RANGE        11    2}
	{P-ENV-TIME-KEYFOLLOW        12    4}
	{P-ENV-TIME-1                13   50}
	{P-ENV-TIME-2                14   50}
	{P-ENV-TIME-3                15   50}
	{P-ENV-TIME-4                16   50}
	{P-ENV-LEVEL-0               17  100}
	{P-ENV-LEVEL-1                18  100}
	{P-ENV-LEVEL-2                19  100}
	{P-ENV-SUSTAIN-LEVEL          20  100}
	{P-ENV-END-LEVEL              21  100}
	{P-MOD-LFO-DEPTH              22  100}
	{P-MOD-LEVER                  23  100}
	{P-MOD-AFTERTOUCH             24  100}
	{LFO1-WAVE-FORM                25    3}
	{LFO1-RATE                     26  100}
	{LFO1-DELAY-TIME               27  100}
	{LFO1-SYNC                     28    2}
	{LFO2-WAVE-FORM                29    3}
	{LFO2-RATE                     30  100}
	{LFO2-DELAY-TIME               31  100}
	{LFO2-SYNC                     32    1}
	{LFO3-WAVE-FORM                33    3}
	{LFO3-RATE                     34  100}
	{LFO3-DELAY-TIME               35  100}
	{LFO3-SYNC                     36    1}
	{LOW-EQ-FREQUENCY              37   15}
	{LOW-EQ-GAIN                   38   24}
	{HIGH-EQ-FREQUENCY             39   21}
	{HIGH-EQ-Q                     40    8}
	{HIGH-EQ-GAIN                  41   24}
	{CHORUS-TYPE                   42    7}
	{CHORUS-RATE                   43  100}
	{CHORUS-DEPTH                  44  100}
	{CHORUS-BALANCE                45  100}
	{PARTIAL-MUTE                  46    3}
	{PARTIAL-BALANCE               47  100}
	{EXTENSION-48                  48  127}
	{EXTENSION-49                  49  127}
	{EXTENSION-50                  50  127}
	{EXTENSION-51                  51  127}
	{EXTENSION-52                  52  127}
	{EXTENSION-53                  53  127}
	{EXTENSION-54                  54  127}
	{EXTENSION-55                  55  127}
	{EXTENSION-56                  56  127}
	{EXTENSION-57                  57  127}
	{EXTENSION-58                  58  127}
	{EXTENSION-59                  59  127}
	{EXTENSION-60                  60  127}
	{EXTENSION-61                  61  127}
	{EXTENSION-62                  62  127}
	{EXTENSION-63                  63  127}
}

# {name  offset(within the 64-byte patch block)  hi} - manual section *4-6, minus offsets 0-17
# (Patch Name, handled separately by d50Name).
set d50PatchFields {
	{KEY-MODE                      18    8}
	{SPLIT-POINT                   19   60}
	{PORTAMENTO-MODE               20    2}
	{HOLD-MODE                     21    2}
	{UPPER-TONE-KEY-SHIFT          22   48}
	{LOWER-TONE-KEY-SHIFT          23   48}
	{UPPER-TONE-FINE-TUNE          24  100}
	{LOWER-TONE-FINE-TUNE          25  100}
	{BENDER-RANGE                  26   12}
	{AFTERTOUCH-BEND-RANGE         27   24}
	{PORTAMENTO-TIME               28  100}
	{OUTPUT-MODE                   29    3}
	{REVERB-TYPE                   30   31}
	{REVERB-BALANCE                31  100}
	{TOTAL-VOLUME                  32  100}
	{TONE-BALANCE                  33  100}
	{CHASE-MODE                    34    2}
	{CHASE-LEVEL                   35  100}
	{CHASE-TIME                    36  100}
	{MIDI-TRANSMIT-CHANNEL         37   16}
	{MIDI-SEPARATE-REV-CHANNEL     38   16}
	{MIDI-TRANSMIT-PROG-CHANGE     39  100}
}

# Reconstructs the D-50 internal Patch Memory table (64 patches x 448 bytes each) from
# whichever DT1 messages in `messages` fall within its address range - raw packed SysEx
# address [0x8000, 0xF000), i.e. [02-00-00] up to (not including) [03-60-00] where Reverb Data
# begins (manual section 4.2). A patch record does NOT align to one DT1 message: Roland caps a
# DT1 message at 256 bytes (manual section 3, "Data set #1") but one patch is 448 bytes, so a
# patch's own data can start or end mid-message - this concatenates every matching message's
# DATA bytes in address order first, then slices the result into 448-byte patches, rather than
# decoding message-by-message the way decodeToneMessage/decodeDX7Message do. Stops at the first
# address gap, so an incomplete/partial dump yields fewer than 64 patches rather than garbage.
# Returns a list of {patchNumber recordBytes} pairs (patchNumber 1-based).
proc decodeD50PatchTable {messages} {
	set kBase 0x8000
	set kEnd 0xF000
	set kPatchSize 448

	set found {}
	foreach m $messages {
		set len [llength $m]
		if {$len < 10} continue
		if {[lindex $m 0] != 0xF0 || [lindex $m end] != 0xF7} continue
		if {[lindex $m 1] != 0x41} continue ;# Roland
		if {[lindex $m 3] != 0x14} continue ;# model: D-50/D-550
		if {[lindex $m 4] != 0x12} continue ;# DT1
		set a1 [expr {[lindex $m 5] & 0x7f}]
		set a2 [expr {[lindex $m 6] & 0x7f}]
		set a3 [expr {[lindex $m 7] & 0x7f}]
		set addr [expr {($a1 << 14) | ($a2 << 7) | $a3}]
		if {$addr < $kBase || $addr >= $kEnd} continue
		set dataLen [expr {$len - 10}]
		lappend found [list $addr [lrange $m 8 [expr {8 + $dataLen - 1}]]]
	}
	if {[llength $found] == 0} { return {} }
	set found [lsort -integer -index 0 $found]

	set startAddr [lindex [lindex $found 0] 0]
	set expectAddr $startAddr
	set buf {}
	foreach f $found {
		lassign $f addr data
		if {$addr != $expectAddr} break ;# gap - only the contiguous prefix is usable
		foreach b $data { lappend buf $b }
		incr expectAddr [llength $data]
	}

	set startPatchNumber [expr {($startAddr - $kBase) / $kPatchSize + 1}]
	set patches {}
	set count [expr {[llength $buf] / $kPatchSize}]
	for {set i 0} {$i < $count} {incr i} {
		lappend patches [list [expr {$startPatchNumber + $i}] \
		                      [lrange $buf [expr {$i * $kPatchSize}] [expr {$i * $kPatchSize + $kPatchSize - 1}]]]
	}
	return $patches
}

# `rec` is one 448-byte D-50 Patch Memory record (decodeD50PatchTable's own output).
proc dumpD50PatchParams {index name rec} {
	global d50PartialFields d50CommonFields d50PatchFields
	puts ""
	puts "==== D-50 #$index  $name ===="
	puts "  Upper Tone: [d50Name $rec 128 10]     Lower Tone: [d50Name $rec 320 10]"

	foreach f $d50PatchFields {
		lassign $f fname off hi
		puts [format "  %-24s %3d   (0-%d)" $fname [lindex $rec [expr {384 + $off}]] $hi]
	}

	foreach {label base} {"Upper Partial 1" 0  "Upper Partial 2" 64
	                       "Lower Partial 1" 192 "Lower Partial 2" 256} {
		puts "  -- $label --"
		foreach f $d50PartialFields {
			lassign $f fname off hi
			puts [format "    %-24s %3d   (0-%d)" $fname [lindex $rec [expr {$base + $off}]] $hi]
		}
	}
	foreach {label base} {"Upper Common" 128 "Lower Common" 320} {
		puts "  -- $label --"
		foreach f $d50CommonFields {
			lassign $f fname off hi
			puts [format "    %-24s %3d   (0-%d)" $fname [lindex $rec [expr {$base + $off}]] $hi]
		}
	}
}

# One-line human description of ANY SysEx message, D-110 tone or not - Alan's request,
# 2026-09-01: know what a file actually is when it's not a D-110 tone dump (a DX7 patch bank,
# say), instead of this tool just silently finding "0 tones" with no explanation. Identifies by
# manufacturer ID (byte 1) and, for the two manufacturers this bothers to know about in more
# detail, the following format/model byte - not a general SysEx library, just enough to name
# the common case a D-110 user is likely to feed this tool by mistake.
proc classifyMessage {msg} {
	set len [llength $msg]
	if {$len < 2} { return "too short to be SysEx" }
	if {[lindex $msg 0] != 0xF0 || [lindex $msg end] != 0xF7} { return "not SysEx (no F0..F7 wrapper)" }
	set mfr [lindex $msg 1]
	# Plain `switch` matches its patterns as LITERAL STRINGS, not as expr-evaluated numbers -
	# "0x41" would never match $mfr's own decimal string form ("65"), a real trap here. `if`
	# with == is fine (its condition runs through expr, which does parse hex literals), so an
	# if/elseif chain is used instead of switch throughout this proc.
	if {$mfr == 0x41} {
		if {$len < 4} { return "Roland SysEx (truncated)" }
		set model [lindex $msg 3]
		if {$model == 0x16} { return "Roland D-110/D-10/D-20/MT-32 family SysEx" }
		if {$model == 0x14} { return "Roland D-50/D-550 SysEx" }
		return [format "Roland SysEx, model byte 0x%02X (not the D-110/MT-32 family)" $model]
	} elseif {$mfr == 0x43} {
		if {$len < 4} { return "Yamaha SysEx (truncated)" }
		set fmt [lindex $msg 3]
		if {$fmt == 9} { return "Yamaha DX7 32-voice bulk dump" }
		if {$fmt == 0} { return "Yamaha DX7 single-voice dump" }
		return [format "Yamaha SysEx, format byte 0x%02X (not recognised as DX7 voice data)" $fmt]
	} elseif {$mfr == 0x42} {
		return "Korg SysEx"
	} elseif {$mfr == 0x40} {
		return "Kawai SysEx"
	} elseif {$mfr == 0x7E} {
		return "Universal Non-Realtime SysEx"
	} elseif {$mfr == 0x7F} {
		return "Universal Realtime SysEx"
	}
	return [format "SysEx, manufacturer ID 0x%02X (unrecognised)" $mfr]
}

proc trimName {rec} {
	set name ""
	for {set c 0} {$c < 10} {incr c} {
		set ch [lindex $rec $c]
		if {$ch < 32 || $ch > 126} { break }
		append name [format %c $ch]
	}
	return [string trim $name]
}

proc dumpToneParams {index name rec} {
	global commonFields partialFields
	puts ""
	puts "==== #$index  $name ===="
	foreach f $commonFields {
		lassign $f fname off hi
		set v [lindex $rec [expr {10 + $off}]]
		puts [format "  %-14s %3d   (0-%d)" $fname $v $hi]
	}
	for {set p 0} {$p < 4} {incr p} {
		set base [expr {14 + 58 * $p}]
		puts "  -- Partial [expr {$p + 1}] --"
		foreach f $partialFields {
			lassign $f fname off hi
			set v [lindex $rec [expr {$base + $off}]]
			puts [format "    %-16s %3d   (0-%d)" $fname $v $hi]
		}
	}
}

# `rec` is a 128-byte PACKED DX7 voice record (decodeDX7Message's own output) - offsets below
# match this script's header comment / the byte table verified against a real bank.
proc dumpDX7VoiceParams {index name rec} {
	global dx7OpFields
	puts ""
	puts "==== DX7 #$index  $name ===="

	set alg [expr {[lindex $rec 110] & 0x1f}]
	puts [format "  %-16s %3d   (0-31)" ALGORITHM $alg]
	set fbByte [lindex $rec 111]
	puts [format "  %-16s %3d   (0-7)" FEEDBACK [expr {$fbByte & 0x07}]]
	puts [format "  %-16s %3d   (0-1)" OSC-KEY-SYNC [expr {($fbByte >> 3) & 0x01}]]
	puts [format "  %-16s %3d   (0-48, 24=center)" TRANSPOSE [lindex $rec 117]]

	foreach {fname off} {
		PITCH-EG-R1 102  PITCH-EG-R2 103  PITCH-EG-R3 104  PITCH-EG-R4 105
		PITCH-EG-L1 106  PITCH-EG-L2 107  PITCH-EG-L3 108  PITCH-EG-L4 109
	} {
		puts [format "  %-16s %3d   (0-99)" $fname [lindex $rec $off]]
	}

	puts [format "  %-16s %3d   (0-99)" LFO-SPEED [lindex $rec 112]]
	puts [format "  %-16s %3d   (0-99)" LFO-DELAY [lindex $rec 113]]
	puts [format "  %-16s %3d   (0-99)" LFO-PITCH-MOD-DEPTH [lindex $rec 114]]
	puts [format "  %-16s %3d   (0-99)" LFO-AMP-MOD-DEPTH [lindex $rec 115]]
	set lfoByte [lindex $rec 116]
	puts [format "  %-16s %3d   (0-1)" LFO-SYNC [expr {$lfoByte & 0x01}]]
	puts [format "  %-16s %3d   (0-5)" LFO-WAVE [expr {($lfoByte >> 1) & 0x07}]]
	puts [format "  %-16s %3d   (0-7)" PITCH-MOD-SENS [expr {($lfoByte >> 4) & 0x07}]]

	for {set op 0} {$op < 6} {incr op} {
		set opNum [expr {6 - $op}] ;# packed order is OP6 first, so slot 0 = operator 6
		set base [expr {$op * 17}]
		puts "  -- Operator $opNum --"
		foreach f $dx7OpFields {
			lassign $f fname off hi
			set v [lindex $rec [expr {$base + $off}]]
			puts [format "    %-18s %3d   (0-%d)" $fname $v $hi]
		}
		set curves [lindex $rec [expr {$base + 11}]]
		puts [format "    %-18s %3d   (0-3)" SCALE-LEFT-CURVE [expr {$curves & 0x03}]]
		puts [format "    %-18s %3d   (0-3)" SCALE-RIGHT-CURVE [expr {($curves >> 2) & 0x03}]]
		set rsd [lindex $rec [expr {$base + 12}]]
		puts [format "    %-18s %3d   (0-7)" OSC-RATE-SCALE [expr {$rsd & 0x07}]]
		puts [format "    %-18s %3d   (0-14, 7=center)" OSC-DETUNE [expr {($rsd >> 3) & 0x0f}]]
		set akvs [lindex $rec [expr {$base + 13}]]
		puts [format "    %-18s %3d   (0-3)" AMP-MOD-SENS [expr {$akvs & 0x03}]]
		puts [format "    %-18s %3d   (0-7)" KEY-VEL-SENS [expr {($akvs >> 2) & 0x07}]]
		puts [format "    %-18s %3d   (0-99)" OUTPUT-LEVEL [lindex $rec [expr {$base + 14}]]]
		set modeCoarse [lindex $rec [expr {$base + 15}]]
		puts [format "    %-18s %3d   (0-1, 0=ratio 1=fixed)" OSC-MODE [expr {$modeCoarse & 0x01}]]
		puts [format "    %-18s %3d   (0-31)" OSC-FREQ-COARSE [expr {($modeCoarse >> 1) & 0x1f}]]
		puts [format "    %-18s %3d   (0-99)" OSC-FREQ-FINE [lindex $rec [expr {$base + 16}]]]
	}
}

# ---- main ----

if {$argc < 1} {
	puts "usage: tclsh sysex_tone_lister.tcl <file.syx> \[all|<index>|<name text>\]"
	exit 1
}
set path [lindex $argv 0]
set selector [expr {$argc > 1 ? [lindex $argv 1] : ""}]

# The whole rest of main is wrapped in a catch just so piping the output into `head`/`grep`
# (closing the pipe early) exits quietly instead of dumping a Tcl stack trace for a plain
# broken-pipe write error.
if {[catch {

set data [readFileBytes $path]
set messages [extractSysexMessages $data]

set tones {}
set dx7Voices {}
foreach m $messages {
	foreach entry [decodeToneMessage $m] {
		lassign $entry slot rec
		lappend tones [list $slot [trimName $rec] $rec]
	}
	foreach rec [decodeDX7Message $m] {
		lappend dx7Voices [list [dx7TrimName $rec] $rec]
	}
}
# Unlike the D-110/DX7 decoders above, D-50 Patch Memory doesn't decode message-by-message (a
# 448-byte patch can straddle two 256-byte DT1 messages) - decodeD50PatchTable needs every
# message at once to reassemble the whole table first. See its own comment.
set d50Patches {}
foreach p [decodeD50PatchTable $messages] {
	lassign $p patchNum rec
	lappend d50Patches [list $patchNum [d50Name $rec 384 18] $rec]
}

set n [llength $tones]
set dx7n [llength $dx7Voices]
set d50n [llength $d50Patches]

puts "$n D-110 tone(s) found in [file tail $path]"
if {$n > $kNumToneSlots} {
	puts "  -> above the $kNumToneSlots-slot Bank I limit; only the first $kNumToneSlots fit in one bank"
}
if {$dx7n > 0} {
	puts "$dx7n Yamaha DX7 voice(s) found in [file tail $path]"
}
if {$d50n > 0} {
	puts "$d50n Roland D-50/D-550 patch(es) found in [file tail $path]"
}

# Message inventory - always computed, but only printed when there's something in the file
# none of the decoders above read at all (a foreign instrument's SysEx, a DX7 single-voice
# edit-buffer dump, or Roland data outside the areas above), so a normal all-D-110/all-DX7/
# all-D-50 file stays quiet about it.
set kRolandToneLabel "Roland D-110/D-10/D-20/MT-32 family SysEx"
set kDX7BulkLabel "Yamaha DX7 32-voice bulk dump"
set kD50Label "Roland D-50/D-550 SysEx"
array set tally {}
set order {}
foreach m $messages {
	set cls [classifyMessage $m]
	if {![info exists tally($cls)]} { lappend order $cls }
	incr tally($cls)
}
set foreign 0
foreach cls $order { if {$cls ni [list $kRolandToneLabel $kDX7BulkLabel $kD50Label]} { set foreign 1 } }
if {$foreign || ($n == 0 && $dx7n == 0 && $d50n == 0)} {
	puts ""
	puts "Message inventory ([llength $messages] SysEx message(s) total):"
	foreach cls $order {
		puts [format "  %3dx  %s" $tally($cls) $cls]
	}
	if {$n == 0 && $dx7n == 0 && $d50n == 0 && [llength $messages] > 0} {
		puts "  (none of these are D-110 Tone Memory writes, DX7 32-voice bulk dumps, or D-50 DT1 writes - this tool only reads those)"
	}
}
puts ""

set idx 0
foreach t $tones {
	incr idx
	lassign $t slot name rec
	puts [format "%3d.  slot %2d  %s" $idx [expr {$slot + 1}] $name]
}
if {$dx7n > 0} {
	if {$n > 0} { puts "" }
	set idx 0
	foreach v $dx7Voices {
		incr idx
		lassign $v name rec
		puts [format "%3d.  %s" $idx $name]
	}
}
if {$d50n > 0} {
	if {$n > 0 || $dx7n > 0} { puts "" }
	set idx 0
	foreach p $d50Patches {
		incr idx
		lassign $p patchNum name rec
		puts [format "%3d.  patch %2d  %s" $idx $patchNum $name]
	}
}

if {$selector ne ""} {
	if {$selector eq "all"} {
		set idx 0
		foreach t $tones {
			incr idx
			lassign $t slot name rec
			dumpToneParams $idx $name $rec
		}
		set idx 0
		foreach v $dx7Voices {
			incr idx
			lassign $v name rec
			dumpDX7VoiceParams $idx $name $rec
		}
		set idx 0
		foreach p $d50Patches {
			incr idx
			lassign $p patchNum name rec
			dumpD50PatchParams $idx $name $rec
		}
	} elseif {[string is integer -strict $selector]} {
		set i [expr {int($selector)}]
		if {$n > 0 && $i >= 1 && $i <= $n} {
			lassign [lindex $tones [expr {$i - 1}]] slot name rec
			dumpToneParams $i $name $rec
		} elseif {$dx7n > 0 && $i >= 1 && $i <= $dx7n} {
			lassign [lindex $dx7Voices [expr {$i - 1}]] name rec
			dumpDX7VoiceParams $i $name $rec
		} elseif {$d50n > 0 && $i >= 1 && $i <= $d50n} {
			lassign [lindex $d50Patches [expr {$i - 1}]] patchNum name rec
			dumpD50PatchParams $i $name $rec
		} else {
			set maxIdx $n
			if {$dx7n > $maxIdx} { set maxIdx $dx7n }
			if {$d50n > $maxIdx} { set maxIdx $d50n }
			puts ""
			puts "index $selector out of range (1-$maxIdx)"
		}
	} else {
		set matched 0
		set idx 0
		foreach t $tones {
			incr idx
			lassign $t slot name rec
			if {[string first [string tolower $selector] [string tolower $name]] >= 0} {
				dumpToneParams $idx $name $rec
				set matched 1
			}
		}
		set idx 0
		foreach v $dx7Voices {
			incr idx
			lassign $v name rec
			if {[string first [string tolower $selector] [string tolower $name]] >= 0} {
				dumpDX7VoiceParams $idx $name $rec
				set matched 1
			}
		}
		set idx 0
		foreach p $d50Patches {
			incr idx
			lassign $p patchNum name rec
			if {[string first [string tolower $selector] [string tolower $name]] >= 0} {
				dumpD50PatchParams $idx $name $rec
				set matched 1
			}
		}
		if {!$matched} {
			puts ""
			puts "no name matches \"$selector\""
		}
	}
}

} err]} {
	if {[string match "*broken pipe*" $err]} { exit 0 }
	puts stderr "error: $err"
	exit 1
}
