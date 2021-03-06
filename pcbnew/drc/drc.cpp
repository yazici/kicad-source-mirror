/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2004-2019 Jean-Pierre Charras, jp.charras at wanadoo.fr
 * Copyright (C) 2014 Dick Hollenbeck, dick@softplc.com
 * Copyright (C) 2017-2020 KiCad Developers, see change_log.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <fctsys.h>
#include <pcb_edit_frame.h>
#include <trigo.h>
#include <board_design_settings.h>
#include <class_edge_mod.h>
#include <class_drawsegment.h>
#include <class_module.h>
#include <class_track.h>
#include <class_pad.h>
#include <class_zone.h>
#include <class_pcb_text.h>
#include <geometry/seg.h>
#include <math_for_graphics.h>
#include <connectivity/connectivity_algo.h>
#include <bitmaps.h>
#include <tool/tool_manager.h>
#include <tools/pcb_actions.h>
#include <tools/pcb_tool_base.h>
#include <tools/zone_filler_tool.h>
#include <kiface_i.h>
#include <pcbnew.h>
#include <drc/drc.h>
#include <netlist_reader/pcb_netlist.h>
#include <math/util.h>      // for KiROUND

#include <dialog_drc.h>
#include <wx/progdlg.h>
#include <board_commit.h>
#include <geometry/shape_arc.h>
#include <drc/drc_item.h>
#include <drc/drc_courtyard_tester.h>
#include <drc/drc_drilled_hole_tester.h>
#include <confirm.h>
#include "drc_rule_parser.h"

DRC::DRC() :
        PCB_TOOL_BASE( "pcbnew.DRCTool" ),
        m_pcbEditorFrame( nullptr ),
        m_pcb( nullptr ),
        m_drcDialog( nullptr ),
        m_rulesFileLastMod( 0 )
{
    // establish initial values for everything:
    m_doPad2PadTest     = true;         // enable pad to pad clearance tests
    m_doUnconnectedTest = true;         // enable unconnected tests
    m_doZonesTest = false;              // disable zone to items clearance tests
    m_doKeepoutTest = true;             // enable keepout areas to items clearance tests
    m_refillZones = false;              // Only fill zones if requested by user.
    m_reportAllTrackErrors = false;
    m_testFootprints = false;

    m_drcRun = false;
    m_footprintsTested = false;
}


DRC::~DRC()
{
    for( DRC_ITEM* unconnectedItem : m_unconnected )
        delete unconnectedItem;

    for( DRC_ITEM* footprintItem : m_footprints )
        delete footprintItem;
}


void DRC::Reset( RESET_REASON aReason )
{
    m_pcbEditorFrame = getEditFrame<PCB_EDIT_FRAME>();

    if( m_pcb != m_pcbEditorFrame->GetBoard() )
    {
        if( m_drcDialog )
            DestroyDRCDialog( wxID_OK );

        m_pcb = m_pcbEditorFrame->GetBoard();
    }

    loadRules();
}


void DRC::ShowDRCDialog( wxWindow* aParent )
{
    bool show_dlg_modal = true;

    // the dialog needs a parent frame. if it is not specified, this is
    // the PCB editor frame specified in DRC class.
    if( !aParent )
    {
        // if any parent is specified, the dialog is modal.
        // if this is the default PCB editor frame, it is not modal
        show_dlg_modal = false;
        aParent = m_pcbEditorFrame;
    }

    Activate();
    m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );

    if( !m_drcDialog )
    {
        m_drcDialog = new DIALOG_DRC( this, m_pcbEditorFrame, aParent );
        updatePointers();

        if( show_dlg_modal )
            m_drcDialog->ShowModal();
        else
            m_drcDialog->Show( true );
    }
    else    // The dialog is just not visible (because the user has double clicked on an error item)
    {
        updatePointers();
        m_drcDialog->Show( true );
    }
}


int DRC::ShowDRCDialog( const TOOL_EVENT& aEvent )
{
    ShowDRCDialog( nullptr );
    return 0;
}


bool DRC::IsDRCDialogShown()
{
    if( m_drcDialog )
        return m_drcDialog->IsShown();

    return false;
}


void DRC::addMarkerToPcb( MARKER_PCB* aMarker )
{
    if( m_pcb->GetDesignSettings().Ignore( aMarker->GetRCItem()->GetErrorCode() ) )
    {
        delete aMarker;
        return;
    }

    BOARD_COMMIT commit( m_pcbEditorFrame );
    commit.Add( aMarker );
    commit.Push( wxEmptyString, false, false );
}


void DRC::DestroyDRCDialog( int aReason )
{
    if( m_drcDialog )
    {
        m_drcDialog->Destroy();
        m_drcDialog = nullptr;
    }
}


int DRC::TestZoneToZoneOutlines()
{
    BOARD*   board = m_pcbEditorFrame->GetBoard();
    int      nerrors = 0;
    wxString msg;

    std::vector<SHAPE_POLY_SET> smoothed_polys;
    smoothed_polys.resize( board->GetAreaCount() );

    for( int ia = 0; ia < board->GetAreaCount(); ia++ )
    {
        ZONE_CONTAINER*    zoneRef = board->GetArea( ia );
        std::set<VECTOR2I> colinearCorners;
        zoneRef->GetColinearCorners( board, colinearCorners );

        zoneRef->BuildSmoothedPoly( smoothed_polys[ia], &colinearCorners );
    }

    // iterate through all areas
    for( int ia = 0; ia < board->GetAreaCount(); ia++ )
    {
        ZONE_CONTAINER* zoneRef = board->GetArea( ia );

        if( !zoneRef->IsOnCopperLayer() )
            continue;

        // If we are testing a single zone, then iterate through all other zones
        // Otherwise, we have already tested the zone combination
        for( int ia2 = ia + 1; ia2 < board->GetAreaCount(); ia2++ )
        {
            ZONE_CONTAINER* zoneToTest = board->GetArea( ia2 );

            if( zoneRef == zoneToTest )
                continue;

            // test for same layer
            if( zoneRef->GetLayer() != zoneToTest->GetLayer() )
                continue;

            // Test for same net
            if( zoneRef->GetNetCode() == zoneToTest->GetNetCode() && zoneRef->GetNetCode() >= 0 )
                continue;

            // test for different priorities
            if( zoneRef->GetPriority() != zoneToTest->GetPriority() )
                continue;

            // test for different types
            if( zoneRef->GetIsKeepout() != zoneToTest->GetIsKeepout() )
                continue;

            // Examine a candidate zone: compare zoneToTest to zoneRef

            // Get clearance used in zone to zone test.  The policy used to
            // obtain that value is now part of the zone object itself by way of
            // ZONE_CONTAINER::GetClearance().
            wxString clearanceSource;
            int      zone2zoneClearance = zoneRef->GetClearance( zoneToTest, &clearanceSource );

            // Keepout areas have no clearance, so set zone2zoneClearance to 1
            // ( zone2zoneClearance = 0  can create problems in test functions)
            if( zoneRef->GetIsKeepout() )
                zone2zoneClearance = 1;

            // test for some corners of zoneRef inside zoneToTest
            for( auto iterator = smoothed_polys[ia].IterateWithHoles(); iterator; iterator++ )
            {
                VECTOR2I currentVertex = *iterator;
                wxPoint pt( currentVertex.x, currentVertex.y );

                if( smoothed_polys[ia2].Contains( currentVertex ) )
                {
                    DRC_ITEM* drcItem = new DRC_ITEM( DRCE_ZONES_INTERSECT );
                    drcItem->SetItems( zoneRef, zoneToTest );

                    MARKER_PCB* marker = new MARKER_PCB( drcItem, pt );
                    addMarkerToPcb( marker );
                    nerrors++;
                }
            }

            // test for some corners of zoneToTest inside zoneRef
            for( auto iterator = smoothed_polys[ia2].IterateWithHoles(); iterator; iterator++ )
            {
                VECTOR2I currentVertex = *iterator;
                wxPoint pt( currentVertex.x, currentVertex.y );

                if( smoothed_polys[ia].Contains( currentVertex ) )
                {
                    DRC_ITEM* drcItem = new DRC_ITEM( DRCE_ZONES_INTERSECT );
                    drcItem->SetItems( zoneToTest, zoneRef );

                    MARKER_PCB* marker = new MARKER_PCB( drcItem, pt );
                    addMarkerToPcb( marker );
                    nerrors++;
                }
            }

            // Iterate through all the segments of refSmoothedPoly
            std::map<wxPoint, int> conflictPoints;

            for( auto refIt = smoothed_polys[ia].IterateSegmentsWithHoles(); refIt; refIt++ )
            {
                // Build ref segment
                SEG refSegment = *refIt;

                // Iterate through all the segments in smoothed_polys[ia2]
                for( auto testIt = smoothed_polys[ia2].IterateSegmentsWithHoles(); testIt; testIt++ )
                {
                    // Build test segment
                    SEG testSegment = *testIt;
                    wxPoint pt;

                    int ax1, ay1, ax2, ay2;
                    ax1 = refSegment.A.x;
                    ay1 = refSegment.A.y;
                    ax2 = refSegment.B.x;
                    ay2 = refSegment.B.y;

                    int bx1, by1, bx2, by2;
                    bx1 = testSegment.A.x;
                    by1 = testSegment.A.y;
                    bx2 = testSegment.B.x;
                    by2 = testSegment.B.y;

                    int d = GetClearanceBetweenSegments( bx1, by1, bx2, by2,
                                                         0,
                                                         ax1, ay1, ax2, ay2,
                                                         0,
                                                         zone2zoneClearance,
                                                         &pt.x, &pt.y );

                    if( d < zone2zoneClearance )
                    {
                        if( conflictPoints.count( pt ) )
                            conflictPoints[ pt ] = std::min( conflictPoints[ pt ], d );
                        else
                            conflictPoints[ pt ] = d;
                    }
                }
            }

            for( const std::pair<const wxPoint, int>& conflict : conflictPoints )
            {
                int       actual = conflict.second;
                DRC_ITEM* drcItem;

                if( actual <= 0 )
                {
                    drcItem = new DRC_ITEM( DRCE_ZONES_INTERSECT );
                }
                else
                {
                    drcItem = new DRC_ITEM( DRCE_ZONES_TOO_CLOSE );

                    msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                                clearanceSource,
                                MessageTextFromValue( userUnits(), zone2zoneClearance, true ),
                                MessageTextFromValue( userUnits(), conflict.second, true ) );

                    drcItem->SetErrorMessage( msg );
                }

                drcItem->SetItems( zoneRef, zoneToTest );

                MARKER_PCB* marker = new MARKER_PCB( drcItem, conflict.first );
                addMarkerToPcb( marker );
                nerrors++;
            }
        }
    }

    return nerrors;
}


void DRC::loadRules()
{
    wxString   rulesFilepath = m_pcbEditorFrame->Prj().AbsolutePath( "drc-rules" );
    wxFileName rulesFile( rulesFilepath );

    if( rulesFile.FileExists() )
    {
        wxLongLong lastMod = rulesFile.GetModificationTime().GetValue();

        if( lastMod > m_rulesFileLastMod )
        {
            m_rulesFileLastMod = lastMod;
            m_ruleSelectors.clear();
            m_rules.clear();

            FILE* fp = wxFopen( rulesFilepath, wxT( "rt" ) );

            if( fp )
            {
                try
                {
                    DRC_RULES_PARSER parser( m_pcb, fp, rulesFilepath );
                    parser.Parse( m_ruleSelectors, m_rules );
                }
                catch( PARSE_ERROR& pe )
                {
                    // Don't leave possibly malformed stuff around for us to trip over
                    m_ruleSelectors.clear();
                    m_rules.clear();

                    DisplayError( m_drcDialog, pe.What() );
                }
            }
        }
    }

    BOARD_DESIGN_SETTINGS& bds = m_pcb->GetDesignSettings();
    bds.m_DRCRuleSelectors = m_ruleSelectors;
    bds.m_DRCRules = m_rules;
}


void DRC::RunTests( wxTextCtrl* aMessages )
{
    loadRules();

    // be sure m_pcb is the current board, not a old one
    // ( the board can be reloaded )
    m_pcb = m_pcbEditorFrame->GetBoard();

    if( aMessages )
    {
        aMessages->AppendText( _( "Board Outline...\n" ) );
        wxSafeYield();
    }

    testOutline();

    if( aMessages )
    {
        aMessages->AppendText( _( "Netclasses...\n" ) );
        wxSafeYield();
    }

    if( !testNetClasses() )
    {
        // testing the netclasses is a special case because if the netclasses
        // do not pass the BOARD_DESIGN_SETTINGS checks, then every member of a net
        // class (a NET) will cause its items such as tracks, vias, and pads
        // to also fail.  So quit after *all* netclass errors have been reported.
        if( aMessages )
            aMessages->AppendText( _( "NETCLASS VIOLATIONS: Aborting DRC\n" ) );

        // update the m_drcDialog listboxes
        updatePointers();

        return;
    }

    // test pad to pad clearances, nothing to do with tracks, vias or zones.
    if( m_doPad2PadTest )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Pad clearances...\n" ) );
            wxSafeYield();
        }

        testPad2Pad();
    }

    // test clearances between drilled holes
    if( aMessages )
    {
        aMessages->AppendText( _( "Drill clearances...\n" ) );
        wxSafeYield();
    }

    testDrilledHoles();

    // caller (a wxTopLevelFrame) is the wxDialog or the Pcb Editor frame that call DRC:
    wxWindow* caller = aMessages ? aMessages->GetParent() : m_pcbEditorFrame;

    if( m_refillZones )
    {
        if( aMessages )
            aMessages->AppendText( _( "Refilling all zones...\n" ) );

        m_toolMgr->GetTool<ZONE_FILLER_TOOL>()->FillAllZones( caller );
    }
    else
    {
        if( aMessages )
            aMessages->AppendText( _( "Checking zone fills...\n" ) );

        m_toolMgr->GetTool<ZONE_FILLER_TOOL>()->CheckAllZones( caller );
    }

    // test track and via clearances to other tracks, pads, and vias
    if( aMessages )
    {
        aMessages->AppendText( _( "Track clearances...\n" ) );
        wxSafeYield();
    }

    testTracks( aMessages ? aMessages->GetParent() : m_pcbEditorFrame, true );

    // test zone clearances to other zones
    if( aMessages )
    {
        aMessages->AppendText( _( "Zone to zone clearances...\n" ) );
        wxSafeYield();
    }

    testZones();

    // find and gather unconnected pads.
    if( m_doUnconnectedTest && !m_pcb->GetDesignSettings().Ignore( DRCE_UNCONNECTED_ITEMS ) )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Unconnected pads...\n" ) );
            aMessages->Refresh();
        }

        testUnconnected();
    }

    // find and gather vias, tracks, pads inside keepout areas.
    if( m_doKeepoutTest )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Keepout areas ...\n" ) );
            aMessages->Refresh();
        }

        testKeepoutAreas();
    }

    // find and gather vias, tracks, pads inside text boxes.
    if( aMessages )
    {
        aMessages->AppendText( _( "Text and graphic clearances...\n" ) );
        wxSafeYield();
    }

    testCopperTextAndGraphics();

    // test courtyards
    if( !m_pcb->GetDesignSettings().Ignore( DRCE_OVERLAPPING_FOOTPRINTS )
        || !m_pcb->GetDesignSettings().Ignore( DRCE_MISSING_COURTYARD )
        || !m_pcb->GetDesignSettings().Ignore( DRCE_MALFORMED_COURTYARD )
        || !m_pcb->GetDesignSettings().Ignore( DRCE_PTH_IN_COURTYARD )
        || !m_pcb->GetDesignSettings().Ignore( DRCE_NPTH_IN_COURTYARD ) )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Courtyard areas...\n" ) );
            aMessages->Refresh();
        }

        doCourtyardsDrc();
    }

    for( DRC_ITEM* footprintItem : m_footprints )
        delete footprintItem;

    m_footprints.clear();
    m_footprintsTested = false;

    if( m_testFootprints && !Kiface().IsSingle() )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Checking footprints against schematic...\n" ) );
            aMessages->Refresh();
        }

        NETLIST netlist;
        m_pcbEditorFrame->FetchNetlistFromSchematic( netlist, PCB_EDIT_FRAME::ANNOTATION_DIALOG );

        if( m_drcDialog )
            m_drcDialog->Raise();

        TestFootprints( netlist, m_pcb, m_drcDialog->GetUserUnits(), m_footprints );
        m_footprintsTested = true;
    }

    // Check if there are items on disabled layers
    if( !m_pcb->GetDesignSettings().Ignore( DRCE_DISABLED_LAYER_ITEM ) )
        testDisabledLayers();

    if( aMessages )
    {
        aMessages->AppendText( _( "Items on disabled layers...\n" ) );
        aMessages->Refresh();
    }

    if( !m_pcb->GetDesignSettings().Ignore( DRCE_UNRESOLVED_VARIABLE ) )
        testTextVars();

    m_drcRun = true;

    // update the m_drcDialog listboxes
    updatePointers();

    if( aMessages )
    {
        // no newline on this one because it is last, don't want the window
        // to unnecessarily scroll.
        aMessages->AppendText( _( "Finished" ) );
    }
}


void DRC::updatePointers()
{
    // update my pointers, m_pcbEditorFrame is the only unchangeable one
    m_pcb = m_pcbEditorFrame->GetBoard();

    m_pcbEditorFrame->ResolveDRCExclusions();

    if( m_drcDialog )  // Use diag list boxes only in DRC dialog
    {
        m_drcDialog->SetMarkersProvider( new BOARD_DRC_ITEMS_PROVIDER( m_pcb ) );
        m_drcDialog->SetUnconnectedProvider( new RATSNEST_DRC_ITEMS_PROVIDER( m_pcbEditorFrame,
                                                                              &m_unconnected ) );
        m_drcDialog->SetFootprintsProvider( new VECTOR_DRC_ITEMS_PROVIDER( m_pcbEditorFrame,
                                                                           &m_footprints ) );
    }
}


bool DRC::doNetClass( const NETCLASSPTR& nc, wxString& msg )
{
    bool ret = true;

    const BOARD_DESIGN_SETTINGS& g = m_pcb->GetDesignSettings();

    if( nc->GetClearance() < g.m_MinClearance )
    {
        DRC_ITEM* drcItem = new DRC_ITEM( DRCE_NETCLASS_CLEARANCE );

        msg.Printf( drcItem->GetErrorText() + _( " (board minimum %s; %s netclass %s)" ),
                    MessageTextFromValue( userUnits(), g.m_MinClearance, true ),
                    nc->GetName(),
                    MessageTextFromValue( userUnits(), nc->GetClearance(), true ) );

        drcItem->SetErrorMessage( msg );
        addMarkerToPcb( new MARKER_PCB( drcItem, wxPoint() ) );
        ret = false;
    }

    if( nc->GetTrackWidth() < g.m_TrackMinWidth )
    {
        DRC_ITEM* drcItem = new DRC_ITEM( DRCE_NETCLASS_TRACKWIDTH );

        msg.Printf( drcItem->GetErrorText() + _( " (board minimum %s; %s netclass %s)" ),
                    MessageTextFromValue( userUnits(), g.m_TrackMinWidth, true ),
                    nc->GetName(),
                    MessageTextFromValue( userUnits(), nc->GetTrackWidth(), true ) );

        drcItem->SetErrorMessage( msg );
        addMarkerToPcb( new MARKER_PCB( drcItem, wxPoint() ) );
        ret = false;
    }

    if( nc->GetViaDiameter() < g.m_ViasMinSize )
    {
        DRC_ITEM* drcItem = new DRC_ITEM( DRCE_NETCLASS_VIASIZE );

        msg.Printf( drcItem->GetErrorText() + _( " (board minimum %s; %s netclass %s)" ),
                    MessageTextFromValue( userUnits(), g.m_ViasMinSize, true ),
                    nc->GetName(),
                    MessageTextFromValue( userUnits(), nc->GetViaDiameter(), true ) );

        drcItem->SetErrorMessage( msg );
        addMarkerToPcb( new MARKER_PCB( drcItem, wxPoint() ) );
        ret = false;
    }

    if( nc->GetViaDrill() < g.m_MinThroughDrill )
    {
        DRC_ITEM* drcItem = new DRC_ITEM( DRCE_NETCLASS_VIADRILLSIZE );

        msg.Printf( drcItem->GetErrorText() + _( " (board min through hole %s; %s netclass %s)" ),
                    MessageTextFromValue( userUnits(), g.m_MinThroughDrill, true ),
                    nc->GetName(),
                    MessageTextFromValue( userUnits(), nc->GetViaDrill(), true ) );

        drcItem->SetErrorMessage( msg );
        addMarkerToPcb( new MARKER_PCB( drcItem, wxPoint() ) );
        ret = false;
    }

    int ncViaAnnulus = ( nc->GetViaDiameter() - nc->GetViaDrill() ) / 2;

    if( ncViaAnnulus < g.m_ViasMinAnnulus )
    {
        DRC_ITEM* drcItem = new DRC_ITEM( DRCE_NETCLASS_VIAANNULUS );

        msg.Printf( drcItem->GetErrorText() + _( " (board minimum %s; %s netclass %s)" ),
                    MessageTextFromValue( userUnits(), g.m_ViasMinAnnulus, true ),
                    nc->GetName(),
                    MessageTextFromValue( userUnits(), ncViaAnnulus, true ) );

        drcItem->SetErrorMessage( msg );
        addMarkerToPcb( new MARKER_PCB( drcItem, wxPoint() ) );
        ret = false;
    }

    if( nc->GetuViaDiameter() < g.m_MicroViasMinSize )
    {
        DRC_ITEM* drcItem = new DRC_ITEM( DRCE_NETCLASS_uVIASIZE );

        msg.Printf( drcItem->GetErrorText() + _( " (board minimum %s; %s netclass %s)" ),
                    MessageTextFromValue( userUnits(), g.m_MicroViasMinSize, true ),
                    nc->GetName(),
                    MessageTextFromValue( userUnits(), nc->GetuViaDiameter(), true ) );

        drcItem->SetErrorMessage( msg );
        addMarkerToPcb( new MARKER_PCB( drcItem, wxPoint() ) );
        ret = false;
    }

    if( nc->GetuViaDrill() < g.m_MicroViasMinDrill )
    {
        DRC_ITEM* drcItem = new DRC_ITEM( DRCE_NETCLASS_uVIADRILLSIZE );

        msg.Printf( drcItem->GetErrorText() + _( " (board minimum %s; %s netclass %s)" ),
                    MessageTextFromValue( userUnits(), g.m_MicroViasMinDrill, true ),
                    nc->GetName(),
                    MessageTextFromValue( userUnits(), nc->GetuViaDrill(), true ) );

        drcItem->SetErrorMessage( msg );
        addMarkerToPcb( new MARKER_PCB( drcItem, wxPoint() ) );
        ret = false;
    }

    return ret;
}


bool DRC::testNetClasses()
{
    bool        ret = true;
    NETCLASSES& netclasses = m_pcb->GetDesignSettings().m_NetClasses;
    wxString    msg;   // construct this only once here, not in a loop, since somewhat expensive.

    if( !doNetClass( netclasses.GetDefault(), msg ) )
        ret = false;

    for( NETCLASSES::const_iterator i = netclasses.begin();  i != netclasses.end();  ++i )
    {
        NETCLASSPTR nc = i->second;

        if( !doNetClass( nc, msg ) )
            ret = false;
    }

    return ret;
}


void DRC::testPad2Pad()
{
    std::vector<D_PAD*> sortedPads;

    m_pcb->GetSortedPadListByXthenYCoord( sortedPads );

    if( sortedPads.empty() )
        return;

    // find the max size of the pads (used to stop the test)
    int max_size = 0;

    for( D_PAD* pad : sortedPads )
    {
        // GetBoundingRadius() is the radius of the minimum sized circle fully containing the pad
        int radius = pad->GetBoundingRadius();

        if( radius > max_size )
            max_size = radius;
    }

    // Upper limit of pad list (limit not included)
    D_PAD** listEnd = &sortedPads[0] + sortedPads.size();

    // Test the pads
    for( auto& pad : sortedPads )
    {
        int x_limit = pad->GetClearance() + pad->GetBoundingRadius() + pad->GetPosition().x;

        doPadToPadsDrc( pad, &pad, listEnd, max_size + x_limit );
    }
}


void DRC::testDrilledHoles()
{
    DRC_DRILLED_HOLE_TESTER tester( [&]( MARKER_PCB* aMarker ) { addMarkerToPcb( aMarker ); } );

    tester.RunDRC( userUnits(), *m_pcb );
}


void DRC::testTracks( wxWindow *aActiveWindow, bool aShowProgressBar )
{
    wxProgressDialog* progressDialog = NULL;
    const int         delta = 500;  // This is the number of tests between 2 calls to the
                                    // progress bar
    int               count = m_pcb->Tracks().size();
    int               deltamax = count/delta;

    if( aShowProgressBar && deltamax > 3 )
    {
        // Do not use wxPD_APP_MODAL style here: it is not necessary and create issues
        // on OSX
        progressDialog = new wxProgressDialog( _( "Track clearances" ), wxEmptyString,
                                               deltamax, aActiveWindow,
                                               wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME );
        progressDialog->Update( 0, wxEmptyString );
    }

    std::shared_ptr<CONNECTIVITY_DATA> connectivity = m_pcb->GetConnectivity();
    BOARD_DESIGN_SETTINGS&             settings = m_pcb->GetDesignSettings();


    if( !m_pcb->GetDesignSettings().Ignore( DRCE_DANGLING_TRACK )
            || !m_pcb->GetDesignSettings().Ignore( DRCE_DANGLING_VIA ) )
    {
        connectivity->Clear();
        connectivity->Build( m_pcb ); // just in case. This really needs to be reliable.
    }

    int ii = 0;
    count = 0;

    for( auto seg_it = m_pcb->Tracks().begin(); seg_it != m_pcb->Tracks().end(); seg_it++ )
    {
        if( ii++ > delta )
        {
            ii = 0;
            count++;

            if( progressDialog )
            {
                if( !progressDialog->Update( count, wxEmptyString ) )
                    break;  // Aborted by user
#ifdef __WXMAC__
                // Work around a dialog z-order issue on OS X
                if( count == deltamax )
                    aActiveWindow->Raise();
#endif
            }
        }

        // Test new segment against tracks and pads, optionally against copper zones
        doTrackDrc( *seg_it, seg_it + 1, m_pcb->Tracks().end(), m_doZonesTest );

        // Test for dangling items
        int code = (*seg_it)->Type() == PCB_VIA_T ? DRCE_DANGLING_VIA : DRCE_DANGLING_TRACK;
        wxPoint pos;

        if( !settings.Ignore( code ) && connectivity->TestTrackEndpointDangling( *seg_it, &pos ) )
        {
            DRC_ITEM* drcItem = new DRC_ITEM( code );
            drcItem->SetItems( *seg_it );

            MARKER_PCB* marker = new MARKER_PCB( drcItem, pos );
            addMarkerToPcb( marker );
        }
    }

    if( progressDialog )
        progressDialog->Destroy();
}


void DRC::testUnconnected()
{
    for( DRC_ITEM* unconnectedItem : m_unconnected )
        delete unconnectedItem;

    m_unconnected.clear();

    auto connectivity = m_pcb->GetConnectivity();

    connectivity->Clear();
    connectivity->Build( m_pcb ); // just in case. This really needs to be reliable.
    connectivity->RecalculateRatsnest();

    std::vector<CN_EDGE> edges;
    connectivity->GetUnconnectedEdges( edges );

    for( const auto& edge : edges )
    {
        DRC_ITEM* item = new DRC_ITEM( DRCE_UNCONNECTED_ITEMS );
        item->SetItems( edge.GetSourceNode()->Parent(), edge.GetTargetNode()->Parent() );
        m_unconnected.push_back( item );
    }
}


void DRC::testZones()
{
    // Test copper areas for valid netcodes
    // if a netcode is < 0 the netname was not found when reading a netlist
    // if a netcode is == 0 the netname is void, and the zone is not connected.
    // This is allowed, but i am not sure this is a good idea
    //
    // In recent Pcbnew versions, the netcode is always >= 0, but an internal net name
    // is stored, and initialized from the file or the zone properties editor.
    // if it differs from the net name from net code, there is a DRC issue
    if( !m_pcb->GetDesignSettings().Ignore( DRCE_ZONE_HAS_EMPTY_NET ) )
    {
        for( int ii = 0; ii < m_pcb->GetAreaCount(); ii++ )
        {
            ZONE_CONTAINER* zone = m_pcb->GetArea( ii );

            if( !zone->IsOnCopperLayer() )
                continue;

            int netcode = zone->GetNetCode();
            // a netcode < 0 or > 0 and no pad in net  is a error or strange
            // perhaps a "dead" net, which happens when all pads in this net were removed
            // Remark: a netcode < 0 should not happen (this is more a bug somewhere)
            int pads_in_net = ( netcode > 0 ) ? m_pcb->GetConnectivity()->GetPadCount( netcode ) : 1;

            if( ( netcode < 0 ) || pads_in_net == 0 )
            {
                DRC_ITEM* drcItem = new DRC_ITEM( DRCE_ZONE_HAS_EMPTY_NET );
                drcItem->SetItems( zone );

                MARKER_PCB* marker = new MARKER_PCB( drcItem, zone->GetPosition() );
                addMarkerToPcb( marker );
            }
        }
    }

    // Test copper areas outlines, and create markers when needed
    TestZoneToZoneOutlines();
}


void DRC::testKeepoutAreas()
{
    // Get a list of all zones to inspect, from both board and footprints
    std::list<ZONE_CONTAINER*> areasToInspect = m_pcb->GetZoneList( true );

    // Test keepout areas for vias, tracks and pads inside keepout areas
    for( ZONE_CONTAINER* area : areasToInspect )
    {
        if( !area->GetIsKeepout() )
            continue;

        for( TRACK* segm : m_pcb->Tracks() )
        {
            if( segm->Type() == PCB_TRACE_T )
            {
                if( !area->GetDoNotAllowTracks()  )
                    continue;

                // Ignore if the keepout zone is not on the same layer
                if( !area->IsOnLayer( segm->GetLayer() ) )
                    continue;

                int         widths = segm->GetWidth() / 2;
                SEG         trackSeg( segm->GetStart(), segm->GetEnd() );
                SEG::ecoord center2center_squared = area->Outline()->SquaredDistance( trackSeg );

                if( center2center_squared <= SEG::Square( widths) )
                {
                    DRC_ITEM* drcItem = new DRC_ITEM( DRCE_TRACK_INSIDE_KEEPOUT );
                    drcItem->SetItems( segm, area );

                    MARKER_PCB* marker = new MARKER_PCB( drcItem, getLocation( segm, area ) );
                    addMarkerToPcb( marker );
                }
            }
            else if( segm->Type() == PCB_VIA_T )
            {
                if( ! area->GetDoNotAllowVias()  )
                    continue;

                if( !area->CommonLayerExists( segm->GetLayerSet() ) )
                    continue;

                int         widths = segm->GetWidth() / 2;
                wxPoint     viaPos = segm->GetPosition();
                SEG::ecoord center2center_squared = area->Outline()->SquaredDistance( viaPos );

                if( center2center_squared <= SEG::Square( widths) )
                {
                    DRC_ITEM* drcItem = new DRC_ITEM( DRCE_VIA_INSIDE_KEEPOUT );
                    drcItem->SetItems( segm, area );

                    MARKER_PCB* marker = new MARKER_PCB( drcItem, getLocation( segm, area ) );
                    addMarkerToPcb( marker );
                }
            }
        }

        if( !area->GetDoNotAllowPads() && !area->GetDoNotAllowFootprints() )
            continue;

        EDA_RECT areaBBox = area->GetBoundingBox();
        bool     checkFront = area->CommonLayerExists( LSET::FrontMask() );
        bool     checkBack = area->CommonLayerExists( LSET::BackMask() );

        for( MODULE* fp : m_pcb->Modules() )
        {
            if( area->GetDoNotAllowFootprints() && ( fp->IsFlipped() ? checkBack : checkFront ) )
            {
                // Fast test to detect a footprint inside the keepout area bounding box.
                if( areaBBox.Intersects( fp->GetBoundingBox() ) )
                {
                    SHAPE_POLY_SET outline;

                    if( fp->BuildPolyCourtyard() )
                    {
                        outline = fp->IsFlipped() ? fp->GetPolyCourtyardBack()
                                                  : fp->GetPolyCourtyardFront();
                    }

                    if( outline.OutlineCount() == 0 )
                        outline = fp->GetBoundingPoly();

                    // Build the common area between footprint and the keepout area:
                    outline.BooleanIntersection( *area->Outline(), SHAPE_POLY_SET::PM_FAST );

                    // If it's not empty then we have a violation
                    if( outline.OutlineCount() )
                    {
                        const VECTOR2I& pt = outline.CVertex( 0, 0, -1 );
                        DRC_ITEM* drcItem = new DRC_ITEM( DRCE_FOOTPRINT_INSIDE_KEEPOUT );
                        drcItem->SetItems( fp, area );

                        MARKER_PCB* marker = new MARKER_PCB( drcItem, (wxPoint) pt );
                        addMarkerToPcb( marker );
                    }
                }
            }

            if( area->GetDoNotAllowPads() )
            {
                for( D_PAD* pad : fp->Pads() )
                {
                    if( !area->CommonLayerExists( pad->GetLayerSet() ) )
                        continue;

                    // Fast test to detect a pad inside the keepout area bounding box.
                    EDA_RECT padBBox( pad->ShapePos(), wxSize() );
                    padBBox.Inflate( pad->GetBoundingRadius() );

                    if( areaBBox.Intersects( padBBox ) )
                    {
                        SHAPE_POLY_SET outline;
                        pad->TransformShapeWithClearanceToPolygon( outline, 0 );

                        // Build the common area between pad and the keepout area:
                        outline.BooleanIntersection( *area->Outline(), SHAPE_POLY_SET::PM_FAST );

                        // If it's not empty then we have a violation
                        if( outline.OutlineCount() )
                        {
                            const VECTOR2I& pt = outline.CVertex( 0, 0, -1 );
                            DRC_ITEM* drcItem = new DRC_ITEM( DRCE_PAD_INSIDE_KEEPOUT );
                            drcItem->SetItems( pad, area );

                            MARKER_PCB* marker = new MARKER_PCB( drcItem, (wxPoint) pt );
                            addMarkerToPcb( marker );
                        }
                    }
                }
            }
        }
    }
}


void DRC::testCopperTextAndGraphics()
{
    // Test copper items for clearance violations with vias, tracks and pads

    for( BOARD_ITEM* brdItem : m_pcb->Drawings() )
    {
        if( IsCopperLayer( brdItem->GetLayer() ) )
        {
            if( brdItem->Type() == PCB_TEXT_T )
                testCopperTextItem( brdItem );
            else if( brdItem->Type() == PCB_LINE_T )
                testCopperDrawItem( static_cast<DRAWSEGMENT*>( brdItem ));
        }
    }

    for( MODULE* module : m_pcb->Modules() )
    {
        TEXTE_MODULE& ref = module->Reference();
        TEXTE_MODULE& val = module->Value();

        if( ref.IsVisible() && IsCopperLayer( ref.GetLayer() ) )
            testCopperTextItem( &ref );

        if( val.IsVisible() && IsCopperLayer( val.GetLayer() ) )
            testCopperTextItem( &val );

        if( module->IsNetTie() )
            continue;

        for( BOARD_ITEM* item : module->GraphicalItems() )
        {
            if( IsCopperLayer( item->GetLayer() ) )
            {
                if( item->Type() == PCB_MODULE_TEXT_T && ( (TEXTE_MODULE*) item )->IsVisible() )
                    testCopperTextItem( item );
                else if( item->Type() == PCB_MODULE_EDGE_T )
                    testCopperDrawItem( static_cast<DRAWSEGMENT*>( item ));
            }
        }
    }
}


void DRC::testCopperDrawItem( DRAWSEGMENT* aItem )
{
    std::vector<SEG> itemShape;
    int              itemWidth = aItem->GetWidth();

    switch( aItem->GetShape() )
    {
    case S_ARC:
    {
        SHAPE_ARC arc( aItem->GetCenter(), aItem->GetArcStart(), (double) aItem->GetAngle() / 10.0 );

        auto l = arc.ConvertToPolyline();

        for( int i = 0; i < l.SegmentCount(); i++ )
            itemShape.push_back( l.Segment( i ) );

        break;
    }

    case S_SEGMENT:
        itemShape.emplace_back( SEG( aItem->GetStart(), aItem->GetEnd() ) );
        break;

    case S_CIRCLE:
    {
        // SHAPE_CIRCLE has no ConvertToPolyline() method, so use a 360.0 SHAPE_ARC
        SHAPE_ARC circle( aItem->GetCenter(), aItem->GetEnd(), 360.0 );

        auto l = circle.ConvertToPolyline();

        for( int i = 0; i < l.SegmentCount(); i++ )
            itemShape.push_back( l.Segment( i ) );

        break;
    }

    case S_CURVE:
    {
        aItem->RebuildBezierToSegmentsPointsList( aItem->GetWidth() );
        wxPoint start_pt = aItem->GetBezierPoints()[0];

        for( unsigned int jj = 1; jj < aItem->GetBezierPoints().size(); jj++ )
        {
            wxPoint end_pt = aItem->GetBezierPoints()[jj];
            itemShape.emplace_back( SEG( start_pt, end_pt ) );
            start_pt = end_pt;
        }

        break;
    }

    default:
        break;
    }

    EDA_RECT   bbox = aItem->GetBoundingBox();
    SHAPE_RECT rect_area( bbox.GetX(), bbox.GetY(), bbox.GetWidth(), bbox.GetHeight() );
    wxString   msg;

    // Test tracks and vias
    for( auto track : m_pcb->Tracks() )
    {
        if( !track->IsOnLayer( aItem->GetLayer() ) )
            continue;

        wxString    clearanceSource;
        int         minClearance = track->GetClearance( nullptr, &clearanceSource );
        int         widths = ( track->GetWidth() + itemWidth ) / 2;
        int         center2centerAllowed = minClearance + widths;

        SEG         trackSeg( track->GetStart(), track->GetEnd() );

        // Fast test to detect a track segment candidate inside the text bounding box
        if( !rect_area.Collide( trackSeg, center2centerAllowed ) )
            continue;

        OPT<SEG>    minSeg;
        SEG::ecoord center2center_squared = 0;

        for( const SEG& itemSeg : itemShape )
        {
            SEG::ecoord thisDist_squared = trackSeg.SquaredDistance( itemSeg );

            if( !minSeg || thisDist_squared < center2center_squared )
            {
                minSeg = itemSeg;
                center2center_squared = thisDist_squared;
            }
        }

        if( center2center_squared < SEG::Square( center2centerAllowed ) )
        {
            int       actual = std::max( 0.0, sqrt( center2center_squared ) - widths );
            int       errorCode = ( track->Type() == PCB_VIA_T ) ? DRCE_VIA_NEAR_COPPER
                                                                 : DRCE_TRACK_NEAR_COPPER;
            DRC_ITEM* drcItem = new DRC_ITEM( errorCode );

            msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                        clearanceSource,
                        MessageTextFromValue( userUnits(), minClearance, true ),
                        MessageTextFromValue( userUnits(), actual, true ) );

            drcItem->SetErrorMessage( msg );
            drcItem->SetItems( track, aItem );

            wxPoint     pos = getLocation( track, minSeg.get() );
            MARKER_PCB* marker = new MARKER_PCB( drcItem, pos );
            addMarkerToPcb( marker );
        }
    }

    // Test pads
    for( auto pad : m_pcb->GetPads() )
    {
        if( !pad->IsOnLayer( aItem->GetLayer() ) )
            continue;

        // Graphic items are allowed to act as net-ties within their own footprint
        if( pad->GetParent() == aItem->GetParent() )
            continue;

        // Fast test to detect a pad candidate inside the text bounding box
        // Finer test (time consumming) is made only for pads near the text.
        int bb_radius = pad->GetBoundingRadius() + pad->GetClearance( nullptr );
        VECTOR2I shape_pos( pad->ShapePos() );

        if( !rect_area.Collide( SEG( shape_pos, shape_pos ), bb_radius ) )
            continue;

        wxString clearanceSource;
        int      minClearance = pad->GetClearance( nullptr, &clearanceSource );
        int      widths = itemWidth / 2;
        int      center2centerAllowed = minClearance + widths;

        SHAPE_POLY_SET padOutline;
        pad->TransformShapeWithClearanceToPolygon( padOutline, 0 );

        OPT<SEG>    minSeg;
        SEG::ecoord center2center_squared = 0;

        for( const SEG& itemSeg : itemShape )
        {
            SEG::ecoord thisCenter2center_squared = padOutline.SquaredDistance( itemSeg );

            if( !minSeg || thisCenter2center_squared < center2center_squared )
            {
                minSeg = itemSeg;
                center2center_squared = thisCenter2center_squared;
            }
        }

        if( center2center_squared < SEG::Square( center2centerAllowed ) )
        {
            int       actual = std::max( 0.0, sqrt( center2center_squared ) - widths );
            DRC_ITEM* drcItem = new DRC_ITEM( DRCE_PAD_NEAR_COPPER );

            msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                        clearanceSource,
                        MessageTextFromValue( userUnits(), minClearance, true ),
                        MessageTextFromValue( userUnits(), actual, true ) );

            drcItem->SetErrorMessage( msg );
            drcItem->SetItems( pad, aItem );

            MARKER_PCB* marker = new MARKER_PCB( drcItem, pad->GetPosition() );
            addMarkerToPcb( marker );
        }
    }
}


void DRC::testCopperTextItem( BOARD_ITEM* aTextItem )
{
    EDA_TEXT* text = dynamic_cast<EDA_TEXT*>( aTextItem );

    if( text == nullptr )
        return;

    std::vector<wxPoint> textShape;      // a buffer to store the text shape (set of segments)
    int                  penWidth = text->GetEffectiveTextPenWidth();

    // So far the bounding box makes up the text-area
    text->TransformTextShapeToSegmentList( textShape );

    if( textShape.size() == 0 )     // Should not happen (empty text?)
        return;

    EDA_RECT   bbox = text->GetTextBox();
    SHAPE_RECT rect_area( bbox.GetX(), bbox.GetY(), bbox.GetWidth(), bbox.GetHeight() );
    wxString   msg;

    // Test tracks and vias
    for( auto track : m_pcb->Tracks() )
    {
        if( !track->IsOnLayer( aTextItem->GetLayer() ) )
            continue;

        wxString clearanceSource;
        int      minClearance = track->GetClearance( nullptr, &clearanceSource );
        int      widths = ( track->GetWidth() + penWidth ) / 2;
        int      center2centerAllowed = minClearance + widths;

        SEG      trackSeg( track->GetStart(), track->GetEnd() );

        // Fast test to detect a track segment candidate inside the text bounding box
        if( !rect_area.Collide( trackSeg, center2centerAllowed ) )
            continue;

        OPT<SEG>    minSeg;
        SEG::ecoord center2center_squared = 0;

        for( unsigned jj = 0; jj < textShape.size(); jj += 2 )
        {
            SEG         textSeg( textShape[jj], textShape[jj+1] );
            SEG::ecoord thisDist_squared = trackSeg.SquaredDistance( textSeg );

            if( !minSeg || thisDist_squared < center2center_squared )
            {
                minSeg = textSeg;
                center2center_squared = thisDist_squared;
            }
        }

        if( center2center_squared < SEG::Square( center2centerAllowed ) )
        {
            int       actual = std::max( 0.0, sqrt( center2center_squared ) - widths );
            int       errorCode = ( track->Type() == PCB_VIA_T ) ? DRCE_VIA_NEAR_COPPER
                                                                 : DRCE_TRACK_NEAR_COPPER;
            DRC_ITEM* drcItem = new DRC_ITEM( errorCode );

            msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                        clearanceSource,
                        MessageTextFromValue( userUnits(), minClearance, true ),
                        MessageTextFromValue( userUnits(), actual, true ) );

            drcItem->SetErrorMessage( msg );
            drcItem->SetItems( track, aTextItem );

            wxPoint     pos = getLocation( track, minSeg.get() );
            MARKER_PCB* marker = new MARKER_PCB( drcItem, pos );
            addMarkerToPcb( marker );
        }
    }

    // Test pads
    for( auto pad : m_pcb->GetPads() )
    {
        if( !pad->IsOnLayer( aTextItem->GetLayer() ) )
            continue;

        // Fast test to detect a pad candidate inside the text bounding box
        // Finer test (time consumming) is made only for pads near the text.
        int bb_radius = pad->GetBoundingRadius() + pad->GetClearance( NULL );
        VECTOR2I shape_pos( pad->ShapePos() );

        if( !rect_area.Collide( SEG( shape_pos, shape_pos ), bb_radius ) )
            continue;

        wxString clearanceSource;
        int      minClearance = pad->GetClearance( nullptr, &clearanceSource );
        int      widths = penWidth / 2;
        int      center2centerAllowed = minClearance + widths;

        SHAPE_POLY_SET padOutline;
        pad->TransformShapeWithClearanceToPolygon( padOutline, 0 );

        OPT<SEG>    minSeg;
        SEG::ecoord center2center_squared = 0;

        for( unsigned jj = 0; jj < textShape.size(); jj += 2 )
        {
            SEG         textSeg( textShape[jj], textShape[jj+1] );
            SEG::ecoord thisCenter2center_squared = padOutline.SquaredDistance( textSeg );

            if( !minSeg || thisCenter2center_squared < center2center_squared )
            {
                minSeg = textSeg;
                center2center_squared = thisCenter2center_squared;
            }
        }

        if( center2center_squared < SEG::Square( center2centerAllowed ) )
        {
            int       actual = std::max( 0.0, sqrt( center2center_squared ) - widths );
            DRC_ITEM* drcItem = new DRC_ITEM( DRCE_PAD_NEAR_COPPER );

            msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                        clearanceSource,
                        MessageTextFromValue( userUnits(), minClearance, true ),
                        MessageTextFromValue( userUnits(), actual, true ) );

            drcItem->SetErrorMessage( msg );
            drcItem->SetItems( pad, aTextItem );

            MARKER_PCB* marker = new MARKER_PCB( drcItem, pad->GetPosition() );
            addMarkerToPcb( marker );
        }
    }
}


void DRC::testOutline()
{
    wxString msg;
    wxPoint  error_loc( m_pcb->GetBoardEdgesBoundingBox().GetPosition() );

    m_board_outlines.RemoveAllContours();

    if( !m_pcb->GetBoardPolygonOutlines( m_board_outlines, nullptr, &error_loc ) )
    {
        DRC_ITEM* drcItem = new DRC_ITEM( DRCE_INVALID_OUTLINE );

        msg.Printf( drcItem->GetErrorText() + _( " (not a closed shape)" ) );

        drcItem->SetErrorMessage( msg );
        drcItem->SetItems( m_pcb );

        MARKER_PCB* marker = new MARKER_PCB( drcItem, error_loc );
        addMarkerToPcb( marker );
    }
}


void DRC::testDisabledLayers()
{
    BOARD*   board = m_pcbEditorFrame->GetBoard();
    wxCHECK( board, /*void*/ );

    LSET     disabledLayers = board->GetEnabledLayers().flip();
    wxString msg;

    // Perform the test only for copper layers
    disabledLayers &= LSET::AllCuMask();

    for( TRACK* track : board->Tracks() )
    {
        if( disabledLayers.test( track->GetLayer() ) )
        {
            DRC_ITEM* drcItem = new DRC_ITEM( DRCE_DISABLED_LAYER_ITEM );

            msg.Printf( drcItem->GetErrorText() + _( "layer %s" ),
                        track->GetLayerName() );

            drcItem->SetErrorMessage( msg );
            drcItem->SetItems( track );

            MARKER_PCB* marker = new MARKER_PCB( drcItem, track->GetPosition() );
            addMarkerToPcb( marker );
        }
    }

    for( MODULE* module : board->Modules() )
    {
        module->RunOnChildren(
                    [&]( BOARD_ITEM* child )
                    {
                        if( disabledLayers.test( child->GetLayer() ) )
                        {
                            DRC_ITEM* drcItem = new DRC_ITEM( DRCE_DISABLED_LAYER_ITEM );

                            msg.Printf( drcItem->GetErrorText() + _( "layer %s" ),
                                        child->GetLayerName() );

                            drcItem->SetErrorMessage( msg );
                            drcItem->SetItems( child );

                            MARKER_PCB* marker = new MARKER_PCB( drcItem, child->GetPosition() );
                            addMarkerToPcb( marker );
                        }
                    } );
    }

    for( ZONE_CONTAINER* zone : board->Zones() )
    {
        if( disabledLayers.test( zone->GetLayer() ) )
        {
            DRC_ITEM* drcItem = new DRC_ITEM( DRCE_DISABLED_LAYER_ITEM );

            msg.Printf( drcItem->GetErrorText() + _( "layer %s" ),
                        zone->GetLayerName() );

            drcItem->SetErrorMessage( msg );
            drcItem->SetItems( zone );

            MARKER_PCB* marker = new MARKER_PCB( drcItem, zone->GetPosition() );
            addMarkerToPcb( marker );
        }
    }
}


void DRC::testTextVars()
{
    BOARD* board = m_pcbEditorFrame->GetBoard();

    for( MODULE* module : board->Modules() )
    {
        module->RunOnChildren(
            [&]( BOARD_ITEM* child )
            {
                if( child->Type() == PCB_MODULE_TEXT_T )
                {
                    TEXTE_MODULE* text = static_cast<TEXTE_MODULE*>( child );

                    if( text->GetShownText().Matches( wxT( "*${*}*" ) ) )
                    {
                        DRC_ITEM* drcItem = new DRC_ITEM( DRCE_UNRESOLVED_VARIABLE );
                        drcItem->SetItems( text );

                        MARKER_PCB* marker = new MARKER_PCB( drcItem, text->GetPosition() );
                        addMarkerToPcb( marker );
                    }
                }
            } );
    }

    for( BOARD_ITEM* drawing : board->Drawings() )
    {
        if( drawing->Type() == PCB_TEXT_T )
        {
            TEXTE_PCB* text = static_cast<TEXTE_PCB*>( drawing );

            if( text->GetShownText().Matches( wxT( "*${*}*" ) ) )
            {
                DRC_ITEM* drcItem = new DRC_ITEM( DRCE_UNRESOLVED_VARIABLE );
                drcItem->SetItems( text );

                MARKER_PCB* marker = new MARKER_PCB( drcItem, text->GetPosition() );
                addMarkerToPcb( marker );
            }
        }
    }
}


bool DRC::doPadToPadsDrc( D_PAD* aRefPad, D_PAD** aStart, D_PAD** aEnd, int x_limit )
{
    const static LSET all_cu = LSET::AllCuMask();

    LSET layerMask = aRefPad->GetLayerSet() & all_cu;

    /* used to test DRC pad to holes: this dummy pad has the size and shape of the hole
     * to test pad to pad hole DRC, using the pad to pad DRC test function.
     * Therefore, this dummy pad is a circle or an oval.
     * A pad must have a parent because some functions expect a non null parent
     * to find the parent board, and some other data
     */
    MODULE  dummymodule( m_pcb );    // Creates a dummy parent
    D_PAD   dummypad( &dummymodule );

    // Ensure the hole is on all copper layers
    dummypad.SetLayerSet( all_cu | dummypad.GetLayerSet() );

    for( D_PAD** pad_list = aStart;  pad_list<aEnd;  ++pad_list )
    {
        D_PAD*   pad = *pad_list;
        wxString msg;

        if( pad == aRefPad )
            continue;

        // We can stop the test when pad->GetPosition().x > x_limit
        // because the list is sorted by X values
        if( pad->GetPosition().x > x_limit )
            break;

        // No problem if pads which are on copper layers are on different copper layers,
        // (pads can be only on a technical layer, to build complex pads)
        // but their hole (if any ) can create DRC error because they are on all
        // copper layers, so we test them
        if( ( pad->GetLayerSet() & layerMask ) == 0 &&
            ( pad->GetLayerSet() & all_cu ) != 0 &&
            ( aRefPad->GetLayerSet() & all_cu ) != 0 )
        {
            // if holes are in the same location and have the same size and shape,
            // this can be accepted
            if( pad->GetPosition() == aRefPad->GetPosition()
                && pad->GetDrillSize() == aRefPad->GetDrillSize()
                && pad->GetDrillShape() == aRefPad->GetDrillShape() )
            {
                if( aRefPad->GetDrillShape() == PAD_DRILL_SHAPE_CIRCLE )
                    continue;

                // for oval holes: must also have the same orientation
                if( pad->GetOrientation() == aRefPad->GetOrientation() )
                    continue;
            }

            /* Here, we must test clearance between holes and pads
             * dummy pad size and shape is adjusted to pad drill size and shape
             */
            if( pad->GetDrillSize().x )
            {
                // pad under testing has a hole, test this hole against pad reference
                dummypad.SetPosition( pad->GetPosition() );
                dummypad.SetSize( pad->GetDrillSize() );
                dummypad.SetShape( pad->GetDrillShape() == PAD_DRILL_SHAPE_OBLONG ?
                                                           PAD_SHAPE_OVAL : PAD_SHAPE_CIRCLE );
                dummypad.SetOrientation( pad->GetOrientation() );

                wxString source;
                int      minClearance = aRefPad->GetClearance( nullptr, &source );
                int      actual;

                if( !checkClearancePadToPad( aRefPad, &dummypad, minClearance, &actual ) )
                {
                    DRC_ITEM* drcItem = new DRC_ITEM( DRCE_HOLE_NEAR_PAD );

                    msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                                source,
                                MessageTextFromValue( userUnits(), minClearance, true ),
                                MessageTextFromValue( userUnits(), actual, true ) );

                    drcItem->SetErrorMessage( msg );
                    drcItem->SetItems( pad, aRefPad );

                    MARKER_PCB* marker = new MARKER_PCB( drcItem, pad->GetPosition() );
                    addMarkerToPcb( marker );
                    return false;
                }
            }

            if( aRefPad->GetDrillSize().x ) // pad reference has a hole
            {
                dummypad.SetPosition( aRefPad->GetPosition() );
                dummypad.SetSize( aRefPad->GetDrillSize() );
                dummypad.SetShape( aRefPad->GetDrillShape() == PAD_DRILL_SHAPE_OBLONG ?
                                                               PAD_SHAPE_OVAL : PAD_SHAPE_CIRCLE );
                dummypad.SetOrientation( aRefPad->GetOrientation() );

                wxString source;
                int      minClearance = pad->GetClearance( nullptr, &source );
                int      actual;

                if( !checkClearancePadToPad( pad, &dummypad, minClearance, &actual ) )
                {
                    DRC_ITEM* drcItem = new DRC_ITEM( DRCE_HOLE_NEAR_PAD );

                    msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                                source,
                                MessageTextFromValue( userUnits(), minClearance, true ),
                                MessageTextFromValue( userUnits(), actual, true ) );

                    drcItem->SetErrorMessage( msg );
                    drcItem->SetItems( aRefPad, pad );

                    MARKER_PCB* marker = new MARKER_PCB( drcItem, aRefPad->GetPosition() );
                    addMarkerToPcb( marker );
                    return false;
                }
            }

            continue;
        }

        // The pad must be in a net (i.e pt_pad->GetNet() != 0 ),
        // But no problem if pads have the same netcode (same net)
        if( pad->GetNetCode() && ( aRefPad->GetNetCode() == pad->GetNetCode() ) )
            continue;

        // if pads are from the same footprint
        if( pad->GetParent() == aRefPad->GetParent() )
        {
            // and have the same pad number ( equivalent pads  )

            // one can argue that this 2nd test is not necessary, that any
            // two pads from a single module are acceptable.  This 2nd test
            // should eventually be a configuration option.
            if( pad->PadNameEqual( aRefPad ) )
                continue;
        }

        // if either pad has no drill and is only on technical layers, not a clearance violation
        if( ( ( pad->GetLayerSet() & layerMask ) == 0 && !pad->GetDrillSize().x ) ||
            ( ( aRefPad->GetLayerSet() & layerMask ) == 0 && !aRefPad->GetDrillSize().x ) )
        {
            continue;
        }

        wxString source;
        int      minClearance = aRefPad->GetClearance( nullptr, &source );
        int      actual;

        if( !checkClearancePadToPad( aRefPad, pad, minClearance, &actual ) )
        {
            DRC_ITEM* drcItem = new DRC_ITEM( DRCE_PAD_NEAR_PAD );

            msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                        source,
                        MessageTextFromValue( userUnits(), minClearance, true ),
                        MessageTextFromValue( userUnits(), actual, true ) );

            drcItem->SetErrorMessage( msg );
            drcItem->SetItems( aRefPad, pad );

            MARKER_PCB* marker = new MARKER_PCB( drcItem, aRefPad->GetPosition() );
            addMarkerToPcb( marker );
            return false;
        }
    }

    return true;
}


void DRC::doCourtyardsDrc()
{
    DRC_COURTYARD_TESTER tester( [&]( MARKER_PCB* aMarker ) { addMarkerToPcb( aMarker ); } );

    tester.RunDRC( userUnits(), *m_pcb );
}


void DRC::TestFootprints( NETLIST& aNetlist, BOARD* aPCB, EDA_UNITS aUnits,
                          std::vector<DRC_ITEM*>& aDRCList )
{
    wxString msg;

    auto comp = []( const MODULE* x, const MODULE* y )
    {
        return x->GetReference().CmpNoCase( y->GetReference() ) < 0;
    };
    auto mods = std::set<MODULE*, decltype( comp )>( comp );

    if( !aPCB->GetDesignSettings().Ignore( DRCE_DUPLICATE_FOOTPRINT ) )
    {
        // Search for duplicate footprints on the board
        for( MODULE* mod : aPCB->Modules() )
        {
            auto ins = mods.insert( mod );

            if( !ins.second )
            {
                DRC_ITEM* item = new DRC_ITEM( DRCE_DUPLICATE_FOOTPRINT );
                item->SetItems( mod, *ins.first );
                aDRCList.push_back( item );
            }
        }
    }

    if( !aPCB->GetDesignSettings().Ignore( DRCE_MISSING_FOOTPRINT ) )
    {
        // Search for component footprints in the netlist but not on the board.
        for( unsigned ii = 0; ii < aNetlist.GetCount(); ii++ )
        {
            COMPONENT* component = aNetlist.GetComponent( ii );
            MODULE*    module = aPCB->FindModuleByReference( component->GetReference() );

            if( module == NULL )
            {
                msg.Printf( _( "Missing footprint %s (%s)" ),
                            component->GetReference(),
                            component->GetValue() );

                DRC_ITEM* item = new DRC_ITEM( DRCE_MISSING_FOOTPRINT );
                item->SetErrorMessage( msg );
                aDRCList.push_back( item );
            }
        }
    }

    if( !aPCB->GetDesignSettings().Ignore( DRCE_EXTRA_FOOTPRINT ) )
    {
        // Search for component footprints found on board but not in netlist.
        for( auto module : mods )
        {
            COMPONENT* component = aNetlist.GetComponentByReference( module->GetReference() );

            if( component == NULL )
            {
                DRC_ITEM* item = new DRC_ITEM( DRCE_EXTRA_FOOTPRINT );
                item->SetItems( module );
                aDRCList.push_back( item );
            }
        }
    }
}


void DRC::setTransitions()
{
    Go( &DRC::ShowDRCDialog,              PCB_ACTIONS::runDRC.MakeEvent() );
}


const int EPSILON = Mils2iu( 5 );


wxPoint DRC::getLocation( TRACK* aTrack, ZONE_CONTAINER* aConflictZone ) const
{
    SHAPE_POLY_SET* conflictOutline;

    if( aConflictZone->IsFilled() )
        conflictOutline = const_cast<SHAPE_POLY_SET*>( &aConflictZone->GetFilledPolysList() );
    else
        conflictOutline = aConflictZone->Outline();

    wxPoint pt1 = aTrack->GetPosition();
    wxPoint pt2 = aTrack->GetEnd();

    // If the mid-point is in the zone, then that's a fine place for the marker
    if( conflictOutline->SquaredDistance( ( pt1 + pt2 ) / 2 ) == 0 )
        return ( pt1 + pt2 ) / 2;

    // Otherwise do a binary search for a "good enough" marker location
    else
    {
        while( GetLineLength( pt1, pt2 ) > EPSILON )
        {
            if( conflictOutline->SquaredDistance( pt1 ) < conflictOutline->SquaredDistance( pt2 ) )
                pt2 = ( pt1 + pt2 ) / 2;
            else
                pt1 = ( pt1 + pt2 ) / 2;
        }

        // Once we're within EPSILON pt1 and pt2 are "equivalent"
        return pt1;
    }
}


wxPoint DRC::getLocation( TRACK* aTrack, const SEG& aConflictSeg ) const
{
    wxPoint pt1 = aTrack->GetPosition();
    wxPoint pt2 = aTrack->GetEnd();

    // Do a binary search along the track for a "good enough" marker location
    while( GetLineLength( pt1, pt2 ) > EPSILON )
    {
        if( aConflictSeg.SquaredDistance( pt1 ) < aConflictSeg.SquaredDistance( pt2 ) )
            pt2 = ( pt1 + pt2 ) / 2;
        else
            pt1 = ( pt1 + pt2 ) / 2;
    }

    // Once we're within EPSILON pt1 and pt2 are "equivalent"
    return pt1;
}


