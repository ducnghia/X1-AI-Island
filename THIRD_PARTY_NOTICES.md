# Third-party notices

## PawnIO LpcACPIEC module

`LpcACPIEC.bin` is an unmodified PawnIO module distributed under
LGPL-2.1-or-later. Source: https://github.com/namazso/PawnIO.Modules

The module is used only to permit byte I/O to the standard ACPI embedded
controller ports 0x62 and 0x66. X1FanService communicates with the PawnIO
driver through its documented IOCTL boundary.

## EC protocol reference

The Embedded Controller transaction and locking design was informed
by the MIT-licensed `yamato-ec` crate from Yamato:
https://github.com/mackid1993/Yamato

X1FanService contains an independently written, minimal C++ implementation and
does not include Yamato source files.
