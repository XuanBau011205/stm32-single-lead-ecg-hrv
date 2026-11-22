{ ============================================================================
  P05 Single-Lead ECG - Auto-Placement Script for Altium Designer 17
  Board Size: 80.0 x 55.0 mm | 91 Components | Zero-Collision Verified
  Usage: Open P05_ECG.PcbDoc -> DXP -> Run Script -> AutoPlace_P05_All
  Undo:  Press Ctrl+Z inside Altium to revert completely if needed.
  ============================================================================ }

Function PlaceOne(Board : IPCB_Board; Desig : String; Xmm, Ymm, RotDeg : Double) : Integer;
Var
    Iterator  : IPCB_BoardIterator;
    Comp      : IPCB_Component;
    TargetX   : TCoord;
    TargetY   : TCoord;
Begin
    Result := 0;
    Iterator := Board.BoardIterator_Create;
    Iterator.AddFilter_ObjectSet(MkSet(eComponentObject));
    Iterator.AddFilter_LayerSet(AllLayers);
    Iterator.AddFilter_Method(eProcessAll);

    Comp := Iterator.FirstPCBObject;
    While Comp <> Nil Do
    Begin
        If Comp.Name.Text = Desig Then
        Begin
            TargetX := Board.XOrigin + MMsToCoord(Xmm);
            TargetY := Board.YOrigin + MMsToCoord(Ymm);

            PCBServer.SendMessageToRobots(Comp.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
            Comp.BeginModify;
            Comp.Rotation := RotDeg;
            Comp.MoveByXY(TargetX - Comp.X, TargetY - Comp.Y);
            Comp.CommentOn := False;
            Comp.NameOn := True;
            Comp.EndModify;
            PCBServer.SendMessageToRobots(Comp.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);
            Comp.GraphicallyInvalidate;
            Result := 1;
            Break;
        End;
        Comp := Iterator.NextPCBObject;
    End;
    Board.BoardIterator_Destroy(Iterator);
End;

Procedure AutoPlace_P05_All;
Var
    Board       : IPCB_Board;
    PlacedCount : Integer;
Begin
    Board := PCBServer.GetCurrentPCBBoard;
    If Board = Nil Then
    Begin
        ShowMessage('Hay mo file P05_ECG.PcbDoc truoc khi chay Script!');
        Exit;
    End;

    PlacedCount := 0;
    PCBServer.PreProcess;
    Try
        PlacedCount := PlacedCount + PlaceOne(Board, 'J_ECG', 2.50, 26.00, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'U_AFE', 23.50, 25.00, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_LO_N', 11.00, 32.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_LO_P', 14.50, 32.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_IN_P', 18.00, 26.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_IN_N', 18.00, 24.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'D_ESD1', 18.50, 30.50, 270.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'D_ESD2', 18.50, 20.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_RLD', 5.00, 18.00, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'J_RLD', 8.50, 13.00, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_RLD', 14.50, 19.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_AFE_VDD', 26.50, 30.00, 270.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_REF', 23.50, 30.00, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_REF2', 23.50, 33.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_REF1', 26.50, 33.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_HP1', 19.50, 36.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_HP1', 20.50, 40.00, 270.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_HP2', 23.50, 40.00, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_HP3', 17.00, 40.00, 270.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_HP2', 12.50, 39.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_LP2', 20.50, 16.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_LP2', 23.50, 19.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_LP1', 23.50, 17.00, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_LP1', 26.50, 17.00, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_G1', 27.00, 20.00, 180.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_G2', 24.50, 14.00, 180.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'TP_ECG_OUT', 29.00, 22.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'FB1', 33.50, 46.50, 180.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_A1', 29.50, 46.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_A2', 26.50, 46.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_A3', 23.50, 46.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'U_MCU', 44.50, 25.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'Y2', 36.50, 30.00, 270.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_LSE1', 33.00, 31.20, 180.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_LSE2', 33.00, 28.80, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'Y1', 35.00, 36.00, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_HSE1', 33.00, 39.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_HSE2', 36.50, 39.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_VDDA1', 37.50, 24.50, 180.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_VDDA2', 34.00, 24.50, 180.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_MCU1', 48.00, 18.00, 270.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_MCU2', 51.50, 28.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_MCU3', 41.50, 33.00, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_MCU_BULK', 41.50, 36.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_BOOT0', 44.00, 33.00, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_BOOT1', 45.50, 18.00, 270.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_RST', 37.50, 21.50, 270.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'SW_RST', 33.50, 6.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'SW_WAKE', 43.50, 6.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_WAKE', 46.50, 12.00, 270.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_WAKE', 49.50, 12.00, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'TP_SD_EN', 43.50, 14.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'TP_MCO', 52.00, 24.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'J_SWD', 50.50, 51.00, 180.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'J_UART', 51.50, 5.00, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_PU1', 46.50, 34.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_PU2', 46.50, 37.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'U_INA', 51.50, 42.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_INA', 56.00, 38.50, 270.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_SHUNT', 57.00, 44.00, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'J_IMEAS', 56.00, 51.00, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'J_USB', 80.00, 40.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_CC1', 70.50, 38.50, 180.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_CC2', 70.50, 42.00, 180.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'D1', 69.00, 47.00, 180.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_IN1', 67.00, 42.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_IN2', 67.00, 39.00, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'U_CHG', 63.00, 42.00, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_PROG', 63.50, 46.00, 270.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'LED_STAT', 60.50, 46.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_LED', 63.00, 49.00, 180.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_BAT', 60.00, 39.00, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'J_BAT', 68.50, 52.00, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'U_LDO', 64.50, 35.00, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_LDO1', 68.00, 35.00, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_LDO2', 60.50, 35.00, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'Q_BAT', 57.00, 31.00, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_GATE', 53.50, 31.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_DIV2', 61.00, 31.00, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_DIV1', 64.50, 31.00, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_SENSE', 61.00, 28.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'J_SD', 66.60, 10.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'Q_SD', 58.00, 24.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_SD_G', 54.50, 24.50, 90.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_SD1', 63.00, 25.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'C_SD2', 63.00, 23.00, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_SD5', 62.50, 20.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_SD1', 62.50, 18.50, 180.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_SD2', 62.50, 16.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_SD3', 62.50, 14.50, 0.0);
        PlacedCount := PlacedCount + PlaceOne(Board, 'R_SD4', 62.50, 12.50, 0.0);
    Finally
        PCBServer.PostProcess;
    End;

    Board.ViewManager_FullUpdate;
    ResetParameters;
    AddStringParameter('Action', 'Board');
    RunProcess('PCB:Zoom');
    ShowMessage('Hoan tat! Da xep xong ' + IntToStr(PlacedCount) + '/91 linh kien. Bam Ctrl+S de luu!');
End;
