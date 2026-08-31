#!/usr/bin/perl
# Emit bcm4313_ucode.c: the actual BCM4313 D11/LCN microcode and its section
# header, embedded byte-for-byte from firmware/brcm/bcm43xx-0.fw and
# bcm43xx_hdr-0.fw (downloaded from linux-firmware).  This makes the driver
# self-contained: no /boot/modules/<fw> file is required at runtime.
use strict; use warnings;

sub carray {
  my ($name, $path, $bytes_per_line) = @_;
  open my $f, "<:raw", $path or die $!; local $/; my $d = <$f>;
  my $sz = length($d);
  my $out = "const unsigned char ${name}[$sz] = {\n";
  my $n = 0;
  for my $b (unpack("C*", $d)) {
    $out .= sprintf("0x%02x,", $b);
    $out .= ((++$n % $bytes_per_line) == 0) ? "\n\t" : "";
  }
  $out .= "\n};\n\n";
  return ($out, length($d));
}

open my $o, ">", "bcm4313_ucode.c" or die $!;
print $o <<'HDR';
/*-
 * bcm4313_ucode.c -- BCM4313 D11/LCN microcode, embedded verbatim.
 *
 * The bytes come from the linux-firmware files:
 *   brcm/bcm43xx-0.fw       (bcm4313_ucode_bin[] -- the LCN microcode blob)
 *   brcm/bcm43xx_hdr-0.fw   (bcm4313_ucode_hdr[] -- firmware_hdr section list)
 *
 * For BCM4313 (D11 core rev 24 / LCN PHY) the LCN section is the one tagged
 * D11UCODE_OVERSIGHT24_LCN (idx 11); its size lives in the section tagged
 * D11UCODE_OVERSIGHT24_LCNSZ (idx 12).  Loading this gives the D11 MAC core
 * its microcode (brcmsmac brcms_ucode_write + brcms_ucode_download).
 *
 * Embedded so the driver is self-contained on FreeBSD.  The source .fw files
 * are also kept under firmware/brcm/ in the source tree for reference.
 *
 * $FreeBSD$
 */
#include <sys/types.h>

/*
 * firmware_hdr section list (struct firmware_hdr {offset,len,idx}).
 * bcm4313_ucode_hdr_sz is given in bytes; it must be a multiple of 12.
 */
HDR
my ($h, $hlen) = carray("bcm4313_ucode_hdr", "firmware/brcm/bcm43xx_hdr-0.fw", 12);
print $o "static const struct { unsigned char hdr[12]; } __attribute__((packed)) x;\n";
# just emit as a flat byte array, parsed into struct bcm4313_fw_hdr at runtime.
print $o $h;
print $o "#define\tBCM4313_UCODE_HDR_SZ\t$hlen\n\n";
my ($b, $blen) = carray("bcm4313_ucode_bin", "firmware/brcm/bcm43xx-0.fw", 12);
print $o $b;
print $o "#define\tBCM4313_UCODE_BIN_SZ\t$blen\n";
close $o;
open my $hx, ">", "bcm4313_ucode.h" or die $!;
print $hx <<HDR;
/* bcm4313_ucode.h -- embedded microcode symbols (see bcm4313_ucode.c). */
#ifndef	_BCM4313_UCODE_H_
#define	_BCM4313_UCODE_H_
extern const unsigned char bcm4313_ucode_bin[$blen];
extern const unsigned char bcm4313_ucode_hdr[$hlen];
#define	BCM4313_UCODE_BIN_SZ	$blen
#define	BCM4313_UCODE_HDR_SZ	$hlen
#endif /* _BCM4313_UCODE_H_ */
HDR
close $hx;
print "wrote bcm4313_ucode.c (hdr=$hlen bin=$blen) + bcm4313_ucode.h\n";