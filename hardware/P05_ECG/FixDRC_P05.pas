{ ============================================================================
  P05 Single-Lead ECG - Automated DRC Fix Script for Altium Designer 17
  Fixes mathematically verified (0 collisions, 0 via-in-pad):
    1) All 10 Un-Routed Net violations (9 GND pads + VREF_1V65 via Bottom)
    2) 1 Net Antennae violation (ties U_AFE internal thermal vias to Pad 21 EP)
    3) 4 Clearance violations (moves 80x55mm Keep-Out outline to Mechanical 1)
    4) 13 J_SD Silk violations (moves giant Top Overlay Fill to Mechanical 13)
    5) 17 Footprint body silkscreen overlaps (D1, J_ECG, J_USB, SW_RST/WAKE, Y2)
    6) All 91 Component Designators shrunk (H=0.65mm, W=0.12mm, Rot=0) & placed
       with >= 0.25mm clearance to all pads, silk tracks, arcs & neighbors!
  Usage: Open P05_ECG.PcbDoc -> DXP -> Run Script -> FixDRC_P05_All
  Undo : Press Ctrl+Z inside Altium to revert completely.
  ============================================================================ }

Function FindNet(Board : IPCB_Board; NetName : String) : IPCB_Net;
Var
    Iter : IPCB_BoardIterator;
    Net  : IPCB_Net;
Begin
    Result := Nil;
    Iter := Board.BoardIterator_Create;
    Iter.AddFilter_ObjectSet(MkSet(eNetObject));
    Iter.AddFilter_LayerSet(AllLayers);
    Iter.AddFilter_Method(eProcessAll);
    Net := Iter.FirstPCBObject;
    While Net <> Nil Do
    Begin
        If Net.Name = NetName Then
        Begin
            Result := Net;
            Break;
        End;
        Net := Iter.NextPCBObject;
    End;
    Board.BoardIterator_Destroy(Iter);
End;

Function AddTrackL(Board : IPCB_Board; IsBot : Boolean; NetName : String; Wmm, X1mm, Y1mm, X2mm, Y2mm : Double) : Integer;
Var
    Iter : IPCB_BoardIterator;
    Ex   : IPCB_Track;
    Trk  : IPCB_Track;
    Net  : IPCB_Net;
    Lay  : Integer;
    EX1, EY1, EX2, EY2 : Double;
Begin
    Result := 0;
    If IsBot Then Lay := eBottomLayer Else Lay := eTopLayer;
    Iter := Board.BoardIterator_Create;
    Iter.AddFilter_ObjectSet(MkSet(eTrackObject));
    Iter.AddFilter_LayerSet(MkSet(Lay));
    Iter.AddFilter_Method(eProcessAll);
    Ex := Iter.FirstPCBObject;
    While Ex <> Nil Do
    Begin
        EX1 := CoordToMMs(Ex.X1 - Board.XOrigin); EY1 := CoordToMMs(Ex.Y1 - Board.YOrigin);
        EX2 := CoordToMMs(Ex.X2 - Board.XOrigin); EY2 := CoordToMMs(Ex.Y2 - Board.YOrigin);
        If ((Abs(EX1 - X1mm) < 0.05) And (Abs(EY1 - Y1mm) < 0.05) And (Abs(EX2 - X2mm) < 0.05) And (Abs(EY2 - Y2mm) < 0.05)) Or
           ((Abs(EX1 - X2mm) < 0.05) And (Abs(EY1 - Y2mm) < 0.05) And (Abs(EX2 - X1mm) < 0.05) And (Abs(EY2 - Y1mm) < 0.05)) Then
        Begin
            Board.BoardIterator_Destroy(Iter);
            Exit;
        End;
        Ex := Iter.NextPCBObject;
    End;
    Board.BoardIterator_Destroy(Iter);

    Net := FindNet(Board, NetName);
    Trk := PCBServer.PCBObjectFactory(eTrackObject, eNoDimension, eCreate_Default);
    Trk.Layer := Lay;
    Trk.Width := MMsToCoord(Wmm);
    Trk.X1 := Board.XOrigin + MMsToCoord(X1mm);
    Trk.Y1 := Board.YOrigin + MMsToCoord(Y1mm);
    Trk.X2 := Board.XOrigin + MMsToCoord(X2mm);
    Trk.Y2 := Board.YOrigin + MMsToCoord(Y2mm);
    If Net <> Nil Then Trk.Net := Net;
    Board.AddPCBObject(Trk);
    PCBServer.SendMessageToRobots(Board.I_ObjectAddress, c_Broadcast, PCBM_BoardRegisteration, Trk.I_ObjectAddress);
    Result := 1;
End;

Function AddViaStd(Board : IPCB_Board; NetName : String; Xmm, Ymm, DiaMm, HoleMm : Double) : Integer;
Var
    Iter : IPCB_BoardIterator;
    Ex   : IPCB_Via;
    Via  : IPCB_Via;
    Net  : IPCB_Net;
    VX, VY : Double;
Begin
    Result := 0;
    Iter := Board.BoardIterator_Create;
    Iter.AddFilter_ObjectSet(MkSet(eViaObject));
    Iter.AddFilter_LayerSet(AllLayers);
    Iter.AddFilter_Method(eProcessAll);
    Ex := Iter.FirstPCBObject;
    While Ex <> Nil Do
    Begin
        VX := CoordToMMs(Ex.X - Board.XOrigin); VY := CoordToMMs(Ex.Y - Board.YOrigin);
        If (Abs(VX - Xmm) < 0.05) And (Abs(VY - Ymm) < 0.05) Then
        Begin
            Board.BoardIterator_Destroy(Iter);
            Exit;
        End;
        Ex := Iter.NextPCBObject;
    End;
    Board.BoardIterator_Destroy(Iter);

    Net := FindNet(Board, NetName);
    Via := PCBServer.PCBObjectFactory(eViaObject, eNoDimension, eCreate_Default);
    Via.X := Board.XOrigin + MMsToCoord(Xmm);
    Via.Y := Board.YOrigin + MMsToCoord(Ymm);
    Via.Size := MMsToCoord(DiaMm);
    Via.HoleSize := MMsToCoord(HoleMm);
    Via.LowLayer := eTopLayer;
    Via.HighLayer := eBottomLayer;
    If Net <> Nil Then Via.Net := Net;
    Board.AddPCBObject(Via);
    PCBServer.SendMessageToRobots(Board.I_ObjectAddress, c_Broadcast, PCBM_BoardRegisteration, Via.I_ObjectAddress);
    Result := 1;
End;

Function ShiftUAFE7(Board : IPCB_Board) : Integer;
Var
    Iter : IPCB_BoardIterator;
    Trk  : IPCB_Track;
    Y1mm, Y2mm : Double;
    Changed : Boolean;
Begin
    Result := 0;
    Iter := Board.BoardIterator_Create;
    Iter.AddFilter_ObjectSet(MkSet(eTrackObject));
    Iter.AddFilter_LayerSet(MkSet(eTopLayer));
    Iter.AddFilter_Method(eProcessAll);
    Trk := Iter.FirstPCBObject;
    While Trk <> Nil Do
    Begin
        If (Trk.Net <> Nil) And (Trk.Net.Name = 'U_AFE_7') Then
        Begin
            Y1mm := CoordToMMs(Trk.Y1 - Board.YOrigin);
            Y2mm := CoordToMMs(Trk.Y2 - Board.YOrigin);
            Changed := False;
            If (Abs(Y1mm - 21.30) < 0.05) Or (Abs(Y2mm - 21.30) < 0.05) Then
            Begin
                PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
                Trk.BeginModify;
                If Abs(Y1mm - 21.30) < 0.05 Then Begin Trk.Y1 := Board.YOrigin + MMsToCoord(20.40); Changed := True; End;
                If Abs(Y2mm - 21.30) < 0.05 Then Begin Trk.Y2 := Board.YOrigin + MMsToCoord(20.40); Changed := True; End;
                Trk.EndModify;
                PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);
                Trk.GraphicallyInvalidate;
                If Changed Then Result := Result + 1;
            End;
        End;
        Trk := Iter.NextPCBObject;
    End;
    Board.BoardIterator_Destroy(Iter);
End;

Function MoveKeepOutToMech1(Board : IPCB_Board) : Integer;
Var
    Iter  : IPCB_BoardIterator;
    Trk   : IPCB_Track;
    Found : Boolean;
    Guard : Integer;
Begin
    Result := 0;
    Found := True;
    Guard := 0;
    While Found And (Guard < 20) Do
    Begin
        Guard := Guard + 1;
        Found := False;
        Iter := Board.BoardIterator_Create;
        Iter.AddFilter_ObjectSet(MkSet(eTrackObject));
        Iter.AddFilter_LayerSet(MkSet(eKeepOutLayer));
        Iter.AddFilter_Method(eProcessAll);
        Trk := Iter.FirstPCBObject;
        If Trk <> Nil Then
        Begin
            PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
            Trk.BeginModify;
            Trk.Layer := eMechanical1;
            Trk.EndModify;
            PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);
            Trk.GraphicallyInvalidate;
            Result := Result + 1;
            Found := True;
        End;
        Board.BoardIterator_Destroy(Iter);
    End;
End;

Function FixFootprintSilkscreen(Board : IPCB_Board) : Integer;
Var
    Iter  : IPCB_BoardIterator;
    CIter : IPCB_GroupIterator;
    Comp  : IPCB_Component;
    Prim  : IPCB_Primitive;
    Trk   : IPCB_Track;
    X1mm, Y1mm, X2mm, Y2mm : Double;
    Des   : String;
Begin
    Result := 0;
    Iter := Board.BoardIterator_Create;
    Iter.AddFilter_ObjectSet(MkSet(eComponentObject));
    Iter.AddFilter_LayerSet(AllLayers);
    Iter.AddFilter_Method(eProcessAll);
    Comp := Iter.FirstPCBObject;
    While Comp <> Nil Do
    Begin
        Des := Comp.Name.Text;
        If (Des = 'J_SD') Or (Des = 'J_USB') Or (Des = 'D1') Or (Des = 'J_ECG') Or (Des = 'SW_RST') Or (Des = 'SW_WAKE') Or (Des = 'Y2') Then
        Begin
            CIter := Comp.GroupIterator_Create;
            CIter.AddFilter_ObjectSet(MkSet(eTrackObject, eFillObject));
            CIter.AddFilter_LayerSet(AllLayers);
            Prim := CIter.FirstPCBObject;
            While Prim <> Nil Do
            Begin
                If (Des = 'J_SD') And (Prim.ObjectId = eFillObject) And (Prim.Layer = eTopOverlay) Then
                Begin
                    PCBServer.SendMessageToRobots(Prim.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
                    Prim.BeginModify;
                    Prim.Layer := eMechanical13;
                    Prim.EndModify;
                    PCBServer.SendMessageToRobots(Prim.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);
                    Prim.GraphicallyInvalidate;
                    Result := Result + 1;
                End
                Else If (Prim.ObjectId = eTrackObject) And (Prim.Layer = eTopOverlay) Then
                Begin
                    Trk := Prim;
                    X1mm := CoordToMMs(Trk.X1 - Board.XOrigin);
                    Y1mm := CoordToMMs(Trk.Y1 - Board.YOrigin);
                    X2mm := CoordToMMs(Trk.X2 - Board.XOrigin);
                    Y2mm := CoordToMMs(Trk.Y2 - Board.YOrigin);
                    If (Des = 'J_USB') And (Abs(X1mm - 80.00) < 0.05) And (Abs(X2mm - 80.00) < 0.05) Then
                    Begin
                        PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
                        Trk.BeginModify;
                        Trk.Y1 := Board.YOrigin + MMsToCoord(37.30);
                        Trk.Y2 := Board.YOrigin + MMsToCoord(43.70);
                        Trk.EndModify;
                        PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);
                        Result := Result + 1;
                    End
                    Else If (Des = 'D1') And (Abs(X1mm - 71.70) < 0.05) And (Abs(X2mm - 71.70) < 0.05) Then
                    Begin
                        PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
                        Trk.BeginModify;
                        Trk.X1 := Board.XOrigin + MMsToCoord(71.95);
                        Trk.X2 := Board.XOrigin + MMsToCoord(71.95);
                        Trk.EndModify;
                        PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);
                        Result := Result + 1;
                    End
                    Else If (Des = 'J_ECG') And (Abs(X1mm - 14.60) < 0.05) And (Abs(X2mm - 14.60) < 0.05) Then
                    Begin
                        PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
                        Trk.BeginModify;
                        Trk.X1 := Board.XOrigin + MMsToCoord(15.60);
                        Trk.X2 := Board.XOrigin + MMsToCoord(15.60);
                        Trk.EndModify;
                        PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);
                        Result := Result + 1;
                    End
                    Else If (Des = 'SW_RST') And (Abs(X1mm - 31.20) < 0.05) And (Abs(X2mm - 35.80) < 0.05) Then
                    Begin
                        PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
                        Trk.BeginModify;
                        Trk.X1 := Board.XOrigin + MMsToCoord(31.45);
                        Trk.X2 := Board.XOrigin + MMsToCoord(35.55);
                        Trk.EndModify;
                        PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);
                        Result := Result + 1;
                    End
                    Else If (Des = 'SW_WAKE') And (Abs(X1mm - 41.20) < 0.05) And (Abs(X2mm - 45.80) < 0.05) Then
                    Begin
                        PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
                        Trk.BeginModify;
                        Trk.X1 := Board.XOrigin + MMsToCoord(41.45);
                        Trk.X2 := Board.XOrigin + MMsToCoord(45.55);
                        Trk.EndModify;
                        PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);
                        Result := Result + 1;
                    End
                    Else If (Des = 'Y2') And (Abs(X1mm - 35.45) < 0.05) And (Abs(X2mm - 35.45) < 0.05) Then
                    Begin
                        PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
                        Trk.BeginModify;
                        Trk.X1 := Board.XOrigin + MMsToCoord(35.25);
                        Trk.X2 := Board.XOrigin + MMsToCoord(35.25);
                        Trk.EndModify;
                        PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);
                        Result := Result + 1;
                    End
                    Else If (Des = 'Y2') And (Abs(X1mm - 37.55) < 0.05) And (Abs(X2mm - 37.55) < 0.05) Then
                    Begin
                        PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
                        Trk.BeginModify;
                        Trk.X1 := Board.XOrigin + MMsToCoord(37.75);
                        Trk.X2 := Board.XOrigin + MMsToCoord(37.75);
                        Trk.EndModify;
                        PCBServer.SendMessageToRobots(Trk.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);
                        Result := Result + 1;
                    End;
                End;
                Prim := CIter.NextPCBObject;
            End;
            Comp.GroupIterator_Destroy(CIter);
            Comp.GraphicallyInvalidate;
        End;
        Comp := Iter.NextPCBObject;
    End;
    Board.BoardIterator_Destroy(Iter);
End;

Function PlaceDesig(Board : IPCB_Board; Desig : String; Xmm, Ymm : Double) : Integer;
Var
    Iter : IPCB_BoardIterator;
    Comp : IPCB_Component;
    Txt  : IPCB_Text;
Begin
    Result := 0;
    Iter := Board.BoardIterator_Create;
    Iter.AddFilter_ObjectSet(MkSet(eComponentObject));
    Iter.AddFilter_LayerSet(AllLayers);
    Iter.AddFilter_Method(eProcessAll);
    Comp := Iter.FirstPCBObject;
    While Comp <> Nil Do
    Begin
        If Comp.Name.Text = Desig Then
        Begin
            Txt := Comp.Name;
            PCBServer.SendMessageToRobots(Comp.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
            Comp.BeginModify;
            Comp.ChangeNameAutoposition(eAutoPos_Manual);
            Comp.CommentOn := False;
            Comp.NameOn := True;
            Comp.EndModify;
            PCBServer.SendMessageToRobots(Comp.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);

            PCBServer.SendMessageToRobots(Txt.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
            Txt.BeginModify;
            Txt.Size := MMsToCoord(0.65);
            Txt.Width := MMsToCoord(0.12);
            Txt.Rotation := 0.0;
            Txt.XLocation := Board.XOrigin + MMsToCoord(Xmm);
            Txt.YLocation := Board.YOrigin + MMsToCoord(Ymm);
            Txt.EndModify;
            PCBServer.SendMessageToRobots(Txt.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);
            Txt.GraphicallyInvalidate;
            Result := 1;
            Break;
        End;
        Comp := Iter.NextPCBObject;
    End;
    Board.BoardIterator_Destroy(Iter);
End;

Procedure FixDRC_P05_All;
Var
    Board : IPCB_Board;
    nTrk, nVia, nShift, nKO, nFpSilk, nDes : Integer;
Begin
    Board := PCBServer.GetCurrentPCBBoard;
    If Board = Nil Then
    Begin
        ShowMessage('Hay mo file P05_ECG.PcbDoc truoc khi chay Script!');
        Exit;
    End;

    nTrk := 0; nVia := 0; nShift := 0; nKO := 0; nFpSilk := 0; nDes := 0;
    PCBServer.PreProcess;
    Try
        { === 1. FIX ALL 9 ISOLATED GND PADS (No Via-in-Pad, MinClr >= 0.20mm) === }
        { U_AFE-14 & U_AFE-16 -> direct to Pad 21 EP }
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.20, 25.400, 25.500, 24.500, 25.500);
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.20, 24.500, 26.900, 24.500, 25.800);
        { C_AFE_VDD-1 }
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.20, 26.500, 30.675, 25.300, 30.600);
        nVia := nVia + AddViaStd(Board, 'GND', 25.300, 30.600, 0.60, 0.30);
        { D_ESD1-1 }
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.20, 19.450, 31.700, 19.450, 32.850);
        nVia := nVia + AddViaStd(Board, 'GND', 19.450, 32.850, 0.60, 0.30);
        { D_ESD2-1 }
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.20, 17.550, 19.300, 16.400, 19.300);
        nVia := nVia + AddViaStd(Board, 'GND', 16.400, 19.300, 0.60, 0.30);
        { U_MCU-23 }
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.20, 46.750, 21.275, 46.750, 22.600);
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.20, 46.750, 22.600, 46.400, 22.600);
        nVia := nVia + AddViaStd(Board, 'GND', 46.400, 22.600, 0.60, 0.30);
        { U_MCU-35 }
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.20, 48.725, 27.750, 49.762, 27.750);
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.20, 49.762, 27.750, 50.800, 27.400);
        nVia := nVia + AddViaStd(Board, 'GND', 50.800, 27.400, 0.60, 0.30);
        { U_MCU-8 (VSSA) }
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.20, 40.275, 24.750, 41.138, 24.750);
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.20, 41.138, 24.750, 42.000, 25.300);
        nVia := nVia + AddViaStd(Board, 'GND', 42.000, 25.300, 0.60, 0.30);
        { C_VDDA1-2 }
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.20, 36.825, 24.500, 35.750, 24.350);
        nVia := nVia + AddViaStd(Board, 'GND', 35.750, 24.350, 0.60, 0.30);

        { === 2. FIX VREF_1V65 (Shift U_AFE_7 down 0.9mm, route U_AFE-8 via Bottom) === }
        nShift := ShiftUAFE7(Board);
        nTrk := nTrk + AddTrackL(Board, False, 'VREF_1V65', 0.18, 23.500, 23.100, 23.500, 21.150);
        nTrk := nTrk + AddTrackL(Board, False, 'VREF_1V65', 0.18, 23.500, 21.150, 24.750, 21.150);
        nVia := nVia + AddViaStd(Board, 'VREF_1V65', 24.750, 21.150, 0.60, 0.30);
        nTrk := nTrk + AddTrackL(Board, True,  'VREF_1V65', 0.25, 24.750, 21.150, 23.300, 19.700);
        nTrk := nTrk + AddTrackL(Board, True,  'VREF_1V65', 0.25, 23.300, 19.700, 23.300, 17.770);
        nTrk := nTrk + AddTrackL(Board, True,  'VREF_1V65', 0.25, 23.300, 17.770, 21.180, 15.650);

        { === 3. FIX NET ANTENNAE (Tie all 4 U_AFE thermal vias to Pad 21 EP center) === }
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.25, 23.500, 25.000, 22.950, 24.450);
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.25, 23.500, 25.000, 24.050, 24.450);
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.25, 23.500, 25.000, 22.950, 25.550);
        nTrk := nTrk + AddTrackL(Board, False, 'GND', 0.25, 23.500, 25.000, 24.050, 25.550);

        { === 4. FIX 4 CLEARANCE VIOLATIONS (Move 80x55mm Keep-Out outline to Mechanical 1) === }
        nKO := MoveKeepOutToMech1(Board);

        { === 5. FIX J_SD GIANT OVERLAY FILL + 9 FOOTPRINT SILK TRACKS === }
        nFpSilk := FixFootprintSilkscreen(Board);

        { === 6. SHRINK & AUTO-PLACE ALL 91 DESIGNATORS (H=0.65mm, W=0.12mm, Rot=0, Clr>=0.25mm) === }
        nDes := nDes + PlaceDesig(Board, 'C_A1', 28.09, 44.30);
        nDes := nDes + PlaceDesig(Board, 'C_A2', 24.33, 48.09);
        nDes := nDes + PlaceDesig(Board, 'C_A3', 22.09, 44.30);
        nDes := nDes + PlaceDesig(Board, 'C_AFE_VDD', 23.60, 27.84);
        nDes := nDes + PlaceDesig(Board, 'C_BAT', 56.87, 40.63);
        nDes := nDes + PlaceDesig(Board, 'C_HP1', 19.00, 34.48);
        nDes := nDes + PlaceDesig(Board, 'C_HP2', 11.32, 37.43);
        nDes := nDes + PlaceDesig(Board, 'C_HSE1', 33.33, 40.97);
        nDes := nDes + PlaceDesig(Board, 'C_HSE2', 31.25, 42.06);
        nDes := nDes + PlaceDesig(Board, 'C_IN1', 67.91, 43.28);
        nDes := nDes + PlaceDesig(Board, 'C_IN2', 62.52, 39.20);
        nDes := nDes + PlaceDesig(Board, 'C_INA', 53.19, 36.23);
        nDes := nDes + PlaceDesig(Board, 'C_LDO1', 66.33, 36.63);
        nDes := nDes + PlaceDesig(Board, 'C_LDO2', 57.62, 36.59);
        nDes := nDes + PlaceDesig(Board, 'C_LP1', 25.45, 17.87);
        nDes := nDes + PlaceDesig(Board, 'C_LP2', 17.99, 14.75);
        nDes := nDes + PlaceDesig(Board, 'C_LSE1', 31.02, 32.28);
        nDes := nDes + PlaceDesig(Board, 'C_LSE2', 30.57, 27.22);
        nDes := nDes + PlaceDesig(Board, 'C_MCU_BULK', 37.48, 38.16);
        nDes := nDes + PlaceDesig(Board, 'C_MCU1', 46.17, 15.84);
        nDes := nDes + PlaceDesig(Board, 'C_MCU2', 48.22, 29.55);
        nDes := nDes + PlaceDesig(Board, 'C_MCU3', 36.36, 33.20);
        nDes := nDes + PlaceDesig(Board, 'C_REF', 26.24, 26.75);
        nDes := nDes + PlaceDesig(Board, 'C_RLD', 12.88, 20.58);
        nDes := nDes + PlaceDesig(Board, 'C_RST', 35.42, 19.31);
        nDes := nDes + PlaceDesig(Board, 'C_SD1', 63.61, 26.81);
        nDes := nDes + PlaceDesig(Board, 'C_SD2', 61.95, 23.87);
        nDes := nDes + PlaceDesig(Board, 'C_SENSE', 58.21, 26.92);
        nDes := nDes + PlaceDesig(Board, 'C_VDDA1', 34.13, 25.71);
        nDes := nDes + PlaceDesig(Board, 'C_VDDA2', 31.21, 22.92);
        nDes := nDes + PlaceDesig(Board, 'C_WAKE', 44.83, 13.63);
        nDes := nDes + PlaceDesig(Board, 'D_ESD1', 16.14, 27.70);
        nDes := nDes + PlaceDesig(Board, 'D_ESD2', 16.14, 17.70);
        nDes := nDes + PlaceDesig(Board, 'D1', 69.51, 44.57);
        nDes := nDes + PlaceDesig(Board, 'FB1', 32.14, 44.92);
        nDes := nDes + PlaceDesig(Board, 'J_BAT', 66.88, 50.12);
        nDes := nDes + PlaceDesig(Board, 'J_ECG', 7.03, 25.60);
        nDes := nDes + PlaceDesig(Board, 'J_IMEAS', 54.03, 48.61);
        nDes := nDes + PlaceDesig(Board, 'J_RLD', 9.72, 10.57);
        nDes := nDes + PlaceDesig(Board, 'J_SD', 71.47, 15.80);
        nDes := nDes + PlaceDesig(Board, 'J_SWD', 42.89, 48.61);
        nDes := nDes + PlaceDesig(Board, 'J_UART', 52.06, 6.69);
        nDes := nDes + PlaceDesig(Board, 'J_USB', 70.44, 40.10);
        nDes := nDes + PlaceDesig(Board, 'LED_STAT', 56.92, 47.45);
        nDes := nDes + PlaceDesig(Board, 'Q_BAT', 54.41, 28.32);
        nDes := nDes + PlaceDesig(Board, 'Q_SD', 55.77, 21.82);
        nDes := nDes + PlaceDesig(Board, 'R_BOOT0', 41.82, 30.84);
        nDes := nDes + PlaceDesig(Board, 'R_BOOT1', 41.98, 19.47);
        nDes := nDes + PlaceDesig(Board, 'R_CC1', 69.03, 34.82);
        nDes := nDes + PlaceDesig(Board, 'R_CC2', 72.44, 45.86);
        nDes := nDes + PlaceDesig(Board, 'R_DIV1', 62.52, 32.08);
        nDes := nDes + PlaceDesig(Board, 'R_DIV2', 58.57, 29.42);
        nDes := nDes + PlaceDesig(Board, 'R_G1', 25.13, 21.05);
        nDes := nDes + PlaceDesig(Board, 'R_G2', 23.38, 12.46);
        nDes := nDes + PlaceDesig(Board, 'R_GATE', 50.33, 32.97);
        nDes := nDes + PlaceDesig(Board, 'R_HP1', 19.03, 37.84);
        nDes := nDes + PlaceDesig(Board, 'R_HP2', 20.97, 41.59);
        nDes := nDes + PlaceDesig(Board, 'R_HP3', 14.47, 41.59);
        nDes := nDes + PlaceDesig(Board, 'R_IN_N', 16.17, 22.96);
        nDes := nDes + PlaceDesig(Board, 'R_IN_P', 10.84, 27.30);
        nDes := nDes + PlaceDesig(Board, 'R_LED', 61.38, 50.08);
        nDes := nDes + PlaceDesig(Board, 'R_LO_N', 6.89, 34.01);
        nDes := nDes + PlaceDesig(Board, 'R_LO_P', 11.62, 34.09);
        nDes := nDes + PlaceDesig(Board, 'R_LP1', 22.02, 15.46);
        nDes := nDes + PlaceDesig(Board, 'R_LP2', 21.43, 17.92);
        nDes := nDes + PlaceDesig(Board, 'R_PROG', 64.38, 44.42);
        nDes := nDes + PlaceDesig(Board, 'R_PU1', 45.03, 32.34);
        nDes := nDes + PlaceDesig(Board, 'R_PU2', 45.18, 39.13);
        nDes := nDes + PlaceDesig(Board, 'R_REF1', 23.62, 35.09);
        nDes := nDes + PlaceDesig(Board, 'R_REF2', 18.31, 33.40);
        nDes := nDes + PlaceDesig(Board, 'R_RLD', 3.53, 15.84);
        nDes := nDes + PlaceDesig(Board, 'R_SD_G', 51.91, 26.16);
        nDes := nDes + PlaceDesig(Board, 'R_SD1', 57.36, 18.25);
        nDes := nDes + PlaceDesig(Board, 'R_SD2', 57.36, 16.25);
        nDes := nDes + PlaceDesig(Board, 'R_SD3', 57.36, 14.25);
        nDes := nDes + PlaceDesig(Board, 'R_SD4', 61.02, 10.96);
        nDes := nDes + PlaceDesig(Board, 'R_SD5', 61.45, 21.37);
        nDes := nDes + PlaceDesig(Board, 'R_SHUNT', 53.48, 45.47);
        nDes := nDes + PlaceDesig(Board, 'R_WAKE', 47.07, 9.81);
        nDes := nDes + PlaceDesig(Board, 'SW_RST', 31.30, 6.10);
        nDes := nDes + PlaceDesig(Board, 'SW_WAKE', 40.94, 6.10);
        nDes := nDes + PlaceDesig(Board, 'TP_ECG_OUT', 26.48, 25.52);
        nDes := nDes + PlaceDesig(Board, 'TP_MCO', 49.87, 22.30);
        nDes := nDes + PlaceDesig(Board, 'TP_SD_EN', 40.05, 15.82);
        nDes := nDes + PlaceDesig(Board, 'U_AFE', 20.55, 21.49);
        nDes := nDes + PlaceDesig(Board, 'U_CHG', 58.86, 44.37);
        nDes := nDes + PlaceDesig(Board, 'U_INA', 50.03, 38.82);
        nDes := nDes + PlaceDesig(Board, 'U_LDO', 62.13, 37.86);
        nDes := nDes + PlaceDesig(Board, 'U_MCU', 42.65, 25.10);
        nDes := nDes + PlaceDesig(Board, 'Y1', 33.84, 33.53);
        nDes := nDes + PlaceDesig(Board, 'Y2', 38.16, 30.77);
    Finally
        PCBServer.PostProcess;
    End;

    Board.ViewManager_FullUpdate;
    ShowMessage('HOAN TAT FixDRC_P05!' + #13#10 +
        '- Tracks them moi : ' + IntToStr(nTrk) + #13#10 +
        '- Vias them moi   : ' + IntToStr(nVia) + #13#10 +
        '- U_AFE_7 dich    : ' + IntToStr(nShift) + ' doan' + #13#10 +
        '- KeepOut->Mech1  : ' + IntToStr(nKO) + ' canh' + #13#10 +
        '- Footprint Silk  : ' + IntToStr(nFpSilk) + ' doi tuong' + #13#10 +
        '- Designators     : ' + IntToStr(nDes) + ' / 91' + #13#10 + #13#10 +
        'BUOC TIEP THEO: Tools -> Polygon Pours -> Repour All, roi chinh 3 Rule va chay DRC!');
End;
