# Every Allegrex instruction form, assembled with psp-as to cross-check psprecore::mips against psp-objdump.
# (scripts/check_allegrex.ps1 assembles this, disassembles it with both tools and compares.)
# Not covered because this psp-as rejects them: vuc2i, vc2i, vmfvc, vmtvc.
        .set noreorder
        .set noat
        .text
start:
        nop
        sll     $t0, $t1, 3
        srl     $t0, $t1, 31
        rotr    $t0, $t1, 7
        sra     $t0, $t1, 1
        sllv    $t0, $t1, $t2
        srlv    $t0, $t1, $t2
        rotrv   $t0, $t1, $t2
        srav    $t0, $t1, $t2
        jr      $ra
        jalr    $t9
        jalr    $t0, $t1
        movz    $t0, $t1, $t2
        movn    $t0, $t1, $t2
        syscall
        syscall 0x1234
        break
        sync
        mfhi    $t0
        mthi    $t0
        mflo    $t0
        mtlo    $t0
        clz     $t0, $t1
        clo     $t0, $t1
        mult    $t0, $t1
        multu   $t0, $t1
        div     $zero, $t0, $t1
        divu    $zero, $t0, $t1
        madd    $t0, $t1
        maddu   $t0, $t1
        msub    $t0, $t1
        msubu   $t0, $t1
        add     $t0, $t1, $t2
        addu    $t0, $t1, $t2
        sub     $t0, $t1, $t2
        subu    $t0, $t1, $t2
        and     $t0, $t1, $t2
        or      $t0, $t1, $t2
        xor     $t0, $t1, $t2
        nor     $t0, $t1, $t2
        slt     $t0, $t1, $t2
        sltu    $t0, $t1, $t2
        max     $t0, $t1, $t2
        min     $t0, $t1, $t2
        bltz    $t0, start
        bgez    $t0, start
        bltzl   $t0, start
        bgezl   $t0, start
        bltzal  $t0, start
        bgezal  $t0, start
        bltzall $t0, start
        bgezall $t0, start
        j       start
        jal     start
        beq     $t0, $t1, start
        bne     $t0, $t1, start
        blez    $t0, start
        bgtz    $t0, start
        addi    $t0, $t1, -5
        addiu   $t0, $t1, -5
        slti    $t0, $t1, 100
        sltiu   $t0, $t1, 100
        andi    $t0, $t1, 0xff
        ori     $t0, $t1, 0xff
        xori    $t0, $t1, 0xff
        lui     $t0, 0x1234
        beql    $t0, $t1, start
        bnel    $t0, $t1, start
        blezl   $t0, start
        bgtzl   $t0, start
        ext     $t0, $t1, 3, 5
        ins     $t0, $t1, 3, 5
        wsbh    $t0, $t1
        wsbw    $t0, $t1
        seb     $t0, $t1
        seh     $t0, $t1
        bitrev  $t0, $t1
        lb      $t0, -4($sp)
        lh      $t0, 2($sp)
        lwl     $t0, 3($sp)
        lw      $t0, 4($sp)
        lbu     $t0, 5($sp)
        lhu     $t0, 6($sp)
        lwr     $t0, 7($sp)
        sb      $t0, 8($sp)
        sh      $t0, 10($sp)
        swl     $t0, 11($sp)
        sw      $t0, 12($sp)
        swr     $t0, 13($sp)
        cache   0x14, 0($a0)
        ll      $t0, 0($a0)
        sc      $t0, 0($a0)
        lwc1    $f2, 16($sp)
        swc1    $f2, 16($sp)
        mfc1    $t0, $f3
        mtc1    $t0, $f3
        cfc1    $t0, $31
        ctc1    $t0, $31
        bc1f    start
        bc1t    start
        bc1fl   start
        bc1tl   start
        add.s   $f0, $f1, $f2
        sub.s   $f0, $f1, $f2
        mul.s   $f0, $f1, $f2
        div.s   $f0, $f1, $f2
        sqrt.s  $f0, $f1
        abs.s   $f0, $f1
        mov.s   $f0, $f1
        neg.s   $f0, $f1
        round.w.s $f0, $f1
        trunc.w.s $f0, $f1
        ceil.w.s $f0, $f1
        floor.w.s $f0, $f1
        cvt.s.w $f0, $f1
        cvt.w.s $f0, $f1
        c.f.s   $f0, $f1
        c.un.s  $f0, $f1
        c.eq.s  $f0, $f1
        c.ueq.s $f0, $f1
        c.olt.s $f0, $f1
        c.ult.s $f0, $f1
        c.ole.s $f0, $f1
        c.ule.s $f0, $f1
        c.sf.s  $f0, $f1
        c.ngle.s $f0, $f1
        c.seq.s $f0, $f1
        c.ngl.s $f0, $f1
        c.lt.s  $f0, $f1
        c.nge.s $f0, $f1
        c.le.s  $f0, $f1
        c.ngt.s $f0, $f1
        # --- VFPU ---
        lv.s    S123.s, 8($a0)
        sv.s    S321.s, -8($a0)
        lv.q    C200.q, 16($a0)
        lv.q    R103.q, 16($a0)
        sv.q    C200.q, 16($a0)
        sv.q    C200.q, 16($a0), wb
        lvl.q   C200.q, 16($a0)
        lvr.q   C200.q, 16($a0)
        svl.q   C200.q, 16($a0)
        svr.q   C200.q, 16($a0)
        mfv     $t0, S123.s
        mtv     $t0, S123.s
        bvf     1, start
        bvt     2, start
        bvfl    3, start
        bvtl    4, start
        vadd.s  S000.s, S100.s, S200.s
        vadd.p  C000.p, C100.p, R200.p
        vadd.t  C000.t, C110.t, R201.t
        vadd.q  C000.q, C100.q, R200.q
        vsub.q  C000.q, C100.q, C200.q
        vsbn.s  S000.s, S100.s, S200.s
        vdiv.t  C000.t, C100.t, C200.t
        vmul.q  C000.q, C100.q, C200.q
        vdot.q  S000.s, C100.q, C200.q
        vscl.t  C000.t, C100.t, S200.s
        vhdp.q  S000.s, C100.q, C200.q
        vcrs.t  C000.t, C100.t, C200.t
        vdet.p  S000.s, C100.p, C200.p
        vcmp.q  FL
        vcmp.q  EQ, C100.q, C200.q
        vcmp.t  LT, C100.t, C200.t
        vcmp.s  EZ, S100.s, S200.s
        vcmp.p  NS, C100.p, C200.p
        vmin.q  C000.q, C100.q, C200.q
        vmax.q  C000.q, C100.q, C200.q
        vscmp.q C000.q, C100.q, C200.q
        vsge.q  C000.q, C100.q, C200.q
        vslt.q  C000.q, C100.q, C200.q
        vmov.q  C000.q, C100.q
        vabs.q  C000.q, C100.q
        vneg.q  C000.q, C100.q
        vidt.q  C000.q
        vsat0.q C000.q, C100.q
        vsat1.q C000.q, C100.q
        vzero.q C000.q
        vone.q  C000.q
        vrcp.q  C000.q, C100.q
        vrsq.q  C000.q, C100.q
        vsin.q  C000.q, C100.q
        vcos.q  C000.q, C100.q
        vexp2.q C000.q, C100.q
        vlog2.q C000.q, C100.q
        vsqrt.q C000.q, C100.q
        vasin.q C000.q, C100.q
        vnrcp.q C000.q, C100.q
        vnsin.q C000.q, C100.q
        vrexp2.q C000.q, C100.q
        vrnds.s S100.s
        vrndi.q C000.q
        vrndf1.q C000.q
        vrndf2.q C000.q
        vf2h.q  C000.p, C100.q
        vh2f.p  C000.q, C100.p
        vsbz.s  S000.s, S100.s
        vlgb.s  S000.s, S100.s
        vus2i.p C000.q, C100.p
        vs2i.p  C000.q, C100.p
        vi2uc.q S000.s, C100.q
        vi2c.q  S000.s, C100.q
        vi2us.q C000.p, C100.q
        vi2s.q  C000.p, C100.q
        vsrt1.q C000.q, C100.q
        vsrt2.q C000.q, C100.q
        vbfy1.q C000.q, C100.q
        vbfy2.q C000.q, C100.q
        vocp.q  C000.q, C100.q
        vsocp.p C000.q, C100.p
        vfad.q  S000.s, C100.q
        vavg.q  S000.s, C100.q
        vsrt3.q C000.q, C100.q
        vsrt4.q C000.q, C100.q
        vsgn.q  C000.q, C100.q
        vt4444.q C000.p, C100.q
        vt5551.q C000.p, C100.q
        vt5650.q C000.p, C100.q
        vcst.q  C000.q, VFPU_PI
        vf2in.q C000.q, C100.q, 1
        vf2iz.q C000.q, C100.q, 2
        vf2iu.q C000.q, C100.q, 3
        vf2id.q C000.q, C100.q, 4
        vi2f.q  C000.q, C100.q, 5
        vcmovt.q C000.q, C100.q, 6
        vcmovf.q C000.q, C100.q, 1
        vwbn.s  S000.s, S100.s, 200
        vpfxs   x, -y, |z|, 1/2
        vpfxt   w, 0, -1, 3
        vpfxd   [0:1], m, [-1:1], 
        viim.s  S100.s, -1234
        vfim.s  S100.s, 0.5
        vmmul.q M000.q, M100.q, M200.q
        vmmul.t M000.t, M100.t, E200.t
        vhtfm2.p C000.p, M100.p, C200.p
        vtfm2.p C000.p, M100.p, C200.p
        vhtfm3.t C000.t, M100.t, C200.t
        vtfm3.t C000.t, M100.t, C200.t
        vhtfm4.q C000.q, M100.q, C200.q
        vtfm4.q C000.q, M100.q, C200.q
        vmscl.q M000.q, M100.q, S200.s
        vcrsp.t C000.t, C100.t, C200.t
        vqmul.q C000.q, C100.q, C200.q
        vmmov.q M000.q, E100.q
        vmidt.p M000.p
        vmzero.t M000.t
        vmone.q M000.q
        vrot.q  C000.q, S100.s, [c, s, s, s]
        vrot.p  C000.p, S100.s, [-s, c]
        vnop
        vsync
        vflush
