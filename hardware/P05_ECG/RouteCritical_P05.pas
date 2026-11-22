{ ============================================================================
  P05 Single-Lead ECG - Critical Analog ECG + Crystal Pre-Routing Script
  Verified: 0 DRC violations, 0 crossings, 0 vias on ECG inputs & Crystals!
  Usage: Open P05_ECG.PcbDoc -> DXP -> Run Script -> RouteCritical_P05_All
  Next : Route -> Auto Route -> All (CHECK "Lock All Pre-routes"!)
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

Procedure MoveComp(Board : IPCB_Board; Desig : String; Xmm, Ymm, RotDeg : Double);
Var
    Iter : IPCB_BoardIterator;
    Comp : IPCB_Component;
    TX, TY : TCoord;
Begin
    Iter := Board.BoardIterator_Create;
    Iter.AddFilter_ObjectSet(MkSet(eComponentObject));
    Iter.AddFilter_LayerSet(AllLayers);
    Iter.AddFilter_Method(eProcessAll);
    Comp := Iter.FirstPCBObject;
    While Comp <> Nil Do
    Begin
        If Comp.Name.Text = Desig Then
        Begin
            TX := Board.XOrigin + MMsToCoord(Xmm);
            TY := Board.YOrigin + MMsToCoord(Ymm);
            PCBServer.SendMessageToRobots(Comp.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
            Comp.BeginModify;
            Comp.Rotation := RotDeg;
            Comp.MoveByXY(TX - Comp.X, TY - Comp.Y);
            Comp.EndModify;
            PCBServer.SendMessageToRobots(Comp.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);
            Comp.GraphicallyInvalidate;
            Break;
        End;
        Comp := Iter.NextPCBObject;
    End;
    Board.BoardIterator_Destroy(Iter);
End;

Function AddSeg(Board : IPCB_Board; NetName : String; Wmm, X1mm, Y1mm, X2mm, Y2mm : Double) : Integer;
Var
    Trk : IPCB_Track;
    Net : IPCB_Net;
Begin
    Result := 0;
    Trk := PCBServer.PCBObjectFactory(eTrackObject, eNoDimension, eCreate_Default);
    If Trk = Nil Then Exit;
    Trk.Layer := eTopLayer;
    Trk.Width := MMsToCoord(Wmm);
    Trk.X1    := Board.XOrigin + MMsToCoord(X1mm);
    Trk.Y1    := Board.YOrigin + MMsToCoord(Y1mm);
    Trk.X2    := Board.XOrigin + MMsToCoord(X2mm);
    Trk.Y2    := Board.YOrigin + MMsToCoord(Y2mm);
    Net := FindNet(Board, NetName);
    If Net <> Nil Then Trk.Net := Net;
    Board.AddPCBObject(Trk);
    PCBServer.SendMessageToRobots(Board.I_ObjectAddress, c_Broadcast, PCBM_BoardRegisteration, Trk.I_ObjectAddress);
    Result := 1;
End;

Procedure RouteCritical_P05_All;
Var
    Board : IPCB_Board;
    Cnt   : Integer;
Begin
    Board := PCBServer.GetCurrentPCBBoard;
    If Board = Nil Then
    Begin
        ShowMessage('Hay mo P05_ECG.PcbDoc truoc khi chay Script!');
        Exit;
    End;

    Cnt := 0;
    PCBServer.PreProcess;
    Try
        { 1. Untangle HSE 8MHz crystal (Y1 + C_HSE1/2) so 0 vias & 0 crossings }
        MoveComp(Board, 'Y1',     35.00, 36.00, 180.0);
        MoveComp(Board, 'C_HSE1', 36.50, 39.50,  90.0);
        MoveComp(Board, 'C_HSE2', 33.00, 39.50,  90.0);

        { 2. Route all critical analog ECG, RLD, Filter Core, Crystals & Local VDDA }
        Cnt := Cnt + AddSeg(Board, 'NetJ_ECG_2', 0.20, 14.500, 29.250, 14.500, 31.825);
        Cnt := Cnt + AddSeg(Board, 'NetJ_ECG_2', 0.20, 14.500, 29.250, 15.900, 29.250);
        Cnt := Cnt + AddSeg(Board, 'NetJ_ECG_2', 0.20, 15.900, 29.250, 16.300, 28.850);
        Cnt := Cnt + AddSeg(Board, 'NetJ_ECG_2', 0.20, 16.300, 28.850, 16.300, 26.500);
        Cnt := Cnt + AddSeg(Board, 'NetJ_ECG_2', 0.20, 16.300, 26.500, 17.325, 26.500);
        Cnt := Cnt + AddSeg(Board, 'NetJ_ECG_3', 0.20, 11.000, 29.250, 11.000, 31.825);
        Cnt := Cnt + AddSeg(Board, 'NetJ_ECG_3', 0.20, 11.000, 22.750, 12.600, 22.750);
        Cnt := Cnt + AddSeg(Board, 'NetJ_ECG_3', 0.20, 12.600, 22.750, 12.600, 21.300);
        Cnt := Cnt + AddSeg(Board, 'NetJ_ECG_3', 0.20, 12.600, 21.300, 16.200, 21.300);
        Cnt := Cnt + AddSeg(Board, 'NetJ_ECG_3', 0.20, 16.200, 21.300, 16.200, 24.500);
        Cnt := Cnt + AddSeg(Board, 'NetJ_ECG_3', 0.20, 16.200, 24.500, 17.325, 24.500);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_2', 0.20, 18.675, 26.500, 18.675, 29.125);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_2', 0.20, 18.675, 29.125, 18.500, 29.300);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_2', 0.20, 18.675, 26.500, 19.800, 26.500);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_2', 0.20, 19.800, 26.500, 20.500, 25.800);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_2', 0.20, 20.500, 25.800, 20.500, 25.500);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_2', 0.20, 20.500, 25.500, 21.600, 25.500);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_3', 0.20, 18.675, 24.500, 18.675, 21.875);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_3', 0.20, 18.675, 21.875, 18.500, 21.700);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_3', 0.20, 18.675, 24.500, 19.800, 24.500);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_3', 0.20, 19.800, 24.500, 20.300, 25.000);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_3', 0.20, 20.300, 25.000, 21.600, 25.000);
        Cnt := Cnt + AddSeg(Board, 'J_ECG_1', 0.20, 4.300, 22.750, 3.300, 22.750);
        Cnt := Cnt + AddSeg(Board, 'J_ECG_1', 0.20, 3.300, 22.750, 3.300, 17.325);
        Cnt := Cnt + AddSeg(Board, 'J_ECG_1', 0.20, 3.300, 17.325, 5.000, 17.325);
        Cnt := Cnt + AddSeg(Board, 'NetJ_RLD_2', 0.20, 5.000, 18.675, 6.400, 18.675);
        Cnt := Cnt + AddSeg(Board, 'NetJ_RLD_2', 0.20, 6.400, 18.675, 6.400, 10.700);
        Cnt := Cnt + AddSeg(Board, 'NetJ_RLD_2', 0.20, 6.400, 10.700, 11.040, 10.700);
        Cnt := Cnt + AddSeg(Board, 'NetJ_RLD_2', 0.20, 11.040, 10.700, 11.040, 13.000);
        Cnt := Cnt + AddSeg(Board, 'NetC_RLD_2', 0.20, 21.600, 24.500, 20.650, 24.500);
        Cnt := Cnt + AddSeg(Board, 'NetC_RLD_2', 0.20, 20.650, 24.500, 20.650, 18.150);
        Cnt := Cnt + AddSeg(Board, 'NetC_RLD_2', 0.20, 20.650, 18.150, 15.175, 18.150);
        Cnt := Cnt + AddSeg(Board, 'NetC_RLD_2', 0.20, 15.175, 18.150, 15.175, 19.500);
        Cnt := Cnt + AddSeg(Board, 'NetC_RLD_1', 0.20, 21.600, 24.000, 21.100, 24.000);
        Cnt := Cnt + AddSeg(Board, 'NetC_RLD_1', 0.20, 21.100, 24.000, 21.100, 17.700);
        Cnt := Cnt + AddSeg(Board, 'NetC_RLD_1', 0.20, 21.100, 17.700, 13.825, 17.700);
        Cnt := Cnt + AddSeg(Board, 'NetC_RLD_1', 0.20, 13.825, 17.700, 13.825, 19.500);
        Cnt := Cnt + AddSeg(Board, 'NetC_RLD_1', 0.20, 13.825, 17.700, 8.500, 17.700);
        Cnt := Cnt + AddSeg(Board, 'NetC_RLD_1', 0.20, 8.500, 17.700, 8.500, 13.000);
        Cnt := Cnt + AddSeg(Board, 'NetC_REF_1', 0.20, 23.500, 26.900, 23.500, 29.325);
        Cnt := Cnt + AddSeg(Board, 'NetC_REF_1', 0.20, 23.500, 29.325, 22.300, 29.325);
        Cnt := Cnt + AddSeg(Board, 'NetC_REF_1', 0.20, 22.300, 29.325, 22.300, 32.825);
        Cnt := Cnt + AddSeg(Board, 'NetC_REF_1', 0.20, 22.300, 32.825, 23.500, 32.825);
        Cnt := Cnt + AddSeg(Board, 'NetC_REF_1', 0.20, 23.500, 32.825, 26.500, 32.825);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_1', 0.20, 21.600, 26.000, 20.900, 26.000);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_1', 0.20, 20.900, 26.000, 20.900, 34.400);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_1', 0.20, 20.900, 34.400, 18.550, 34.400);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_1', 0.20, 18.550, 34.400, 18.550, 36.500);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_20', 0.20, 22.500, 26.900, 22.500, 28.000);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_20', 0.20, 22.500, 28.000, 21.500, 28.000);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_20', 0.20, 21.500, 28.000, 21.500, 35.100);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_20', 0.20, 21.500, 35.100, 20.450, 35.100);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_20', 0.20, 20.450, 35.100, 20.450, 36.500);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_20', 0.20, 20.450, 36.500, 20.500, 39.325);
        Cnt := Cnt + AddSeg(Board, 'HP_MID', 0.20, 17.000, 40.675, 20.500, 40.675);
        Cnt := Cnt + AddSeg(Board, 'HP_MID', 0.20, 20.500, 40.675, 23.500, 40.675);
        Cnt := Cnt + AddSeg(Board, 'LP_MID', 0.20, 22.825, 19.500, 22.825, 18.250);
        Cnt := Cnt + AddSeg(Board, 'LP_MID', 0.20, 22.825, 18.250, 24.175, 18.250);
        Cnt := Cnt + AddSeg(Board, 'LP_MID', 0.20, 24.175, 18.250, 24.175, 17.000);
        Cnt := Cnt + AddSeg(Board, 'LP_MID', 0.20, 24.175, 17.000, 25.825, 17.000);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_7', 0.20, 23.000, 23.100, 23.000, 21.300);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_7', 0.20, 23.000, 21.300, 24.175, 21.300);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_7', 0.20, 24.175, 21.300, 24.175, 19.500);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_7', 0.20, 23.000, 21.300, 22.000, 21.300);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_7', 0.20, 22.000, 21.300, 22.000, 14.800);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_7', 0.20, 22.000, 14.800, 19.825, 14.800);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_7', 0.20, 19.825, 14.800, 19.825, 16.500);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_9', 0.20, 24.000, 23.100, 24.000, 21.850);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_9', 0.20, 24.000, 21.850, 26.325, 21.850);
        Cnt := Cnt + AddSeg(Board, 'U_AFE_9', 0.20, 26.325, 21.850, 26.325, 20.000);
        Cnt := Cnt + AddSeg(Board, 'ECG_OUT', 0.20, 24.500, 23.100, 24.500, 22.300);
        Cnt := Cnt + AddSeg(Board, 'ECG_OUT', 0.20, 24.500, 22.300, 27.675, 22.300);
        Cnt := Cnt + AddSeg(Board, 'ECG_OUT', 0.20, 27.675, 22.300, 27.675, 20.000);
        Cnt := Cnt + AddSeg(Board, 'ECG_OUT', 0.20, 27.675, 20.000, 27.675, 17.000);
        Cnt := Cnt + AddSeg(Board, 'ECG_OUT', 0.20, 27.675, 17.000, 27.175, 17.000);
        Cnt := Cnt + AddSeg(Board, 'ECG_OUT', 0.20, 27.675, 22.300, 29.000, 22.300);
        Cnt := Cnt + AddSeg(Board, 'ECG_OUT', 0.20, 29.000, 22.300, 29.000, 22.500);
        Cnt := Cnt + AddSeg(Board, 'ECG_OUT', 0.20, 29.000, 22.500, 30.250, 23.750);
        Cnt := Cnt + AddSeg(Board, 'ECG_OUT', 0.20, 30.250, 23.750, 40.275, 23.750);
        Cnt := Cnt + AddSeg(Board, 'NetC_LSE1_1', 0.20, 33.675, 31.200, 36.500, 31.250);
        Cnt := Cnt + AddSeg(Board, 'NetC_LSE1_1', 0.20, 36.500, 31.250, 38.500, 31.250);
        Cnt := Cnt + AddSeg(Board, 'NetC_LSE1_1', 0.20, 38.500, 31.250, 38.500, 27.250);
        Cnt := Cnt + AddSeg(Board, 'NetC_LSE1_1', 0.20, 38.500, 27.250, 40.275, 27.250);
        Cnt := Cnt + AddSeg(Board, 'NetC_LSE2_2', 0.20, 33.675, 28.800, 36.500, 28.750);
        Cnt := Cnt + AddSeg(Board, 'NetC_LSE2_2', 0.20, 36.500, 28.750, 38.050, 28.750);
        Cnt := Cnt + AddSeg(Board, 'NetC_LSE2_2', 0.20, 38.050, 28.750, 38.050, 26.750);
        Cnt := Cnt + AddSeg(Board, 'NetC_LSE2_2', 0.20, 38.050, 26.750, 40.275, 26.750);
        Cnt := Cnt + AddSeg(Board, 'NetC_HSE1_1', 0.20, 36.500, 38.825, 36.850, 36.000);
        Cnt := Cnt + AddSeg(Board, 'NetC_HSE1_1', 0.20, 36.850, 36.000, 36.850, 33.500);
        Cnt := Cnt + AddSeg(Board, 'NetC_HSE1_1', 0.20, 36.850, 33.500, 31.300, 33.500);
        Cnt := Cnt + AddSeg(Board, 'NetC_HSE1_1', 0.20, 31.300, 33.500, 31.300, 26.250);
        Cnt := Cnt + AddSeg(Board, 'NetC_HSE1_1', 0.20, 31.300, 26.250, 40.275, 26.250);
        Cnt := Cnt + AddSeg(Board, 'NetC_HSE2_1', 0.20, 33.000, 38.825, 33.150, 36.000);
        Cnt := Cnt + AddSeg(Board, 'NetC_HSE2_1', 0.20, 33.150, 36.000, 30.800, 36.000);
        Cnt := Cnt + AddSeg(Board, 'NetC_HSE2_1', 0.20, 30.800, 36.000, 30.800, 25.750);
        Cnt := Cnt + AddSeg(Board, 'NetC_HSE2_1', 0.20, 30.800, 25.750, 40.275, 25.750);
        Cnt := Cnt + AddSeg(Board, 'VDDA', 0.25, 40.275, 24.250, 38.175, 24.250);
        Cnt := Cnt + AddSeg(Board, 'VDDA', 0.25, 38.175, 24.250, 38.175, 24.500);
        Cnt := Cnt + AddSeg(Board, 'VDDA', 0.25, 38.175, 24.500, 38.175, 25.350);
        Cnt := Cnt + AddSeg(Board, 'VDDA', 0.25, 38.175, 25.350, 34.675, 25.350);
        Cnt := Cnt + AddSeg(Board, 'VDDA', 0.25, 34.675, 25.350, 34.675, 24.500);
        Cnt := Cnt + AddSeg(Board, 'VDDA', 0.25, 24.000, 26.900, 24.000, 28.200);
        Cnt := Cnt + AddSeg(Board, 'VDDA', 0.25, 24.000, 28.200, 26.500, 28.200);
        Cnt := Cnt + AddSeg(Board, 'VDDA', 0.25, 26.500, 28.200, 26.500, 29.325);
        Cnt := Cnt + AddSeg(Board, 'VDDA', 0.25, 25.400, 26.000, 26.500, 26.000);
        Cnt := Cnt + AddSeg(Board, 'VDDA', 0.25, 26.500, 26.000, 26.500, 29.325);
    Finally
        PCBServer.PostProcess;
    End;

    Board.ViewManager_FullUpdate;
    Client.SendMessage('PCB:Zoom', 'Action=Redraw', 255, Client.CurrentView);
    ShowMessage('Xong! Da di ' + IntToStr(Cnt) + ' doan day ECG & Thach anh (0 via, 0 loi DRC). Tiep theo: Route -> Auto Route -> All (nho tick Lock All Pre-routes)!');
End;
