/*************************************************************/
/*  board editor: undo and redo functions for board editor  */
/*************************************************************/

#include "fctsys.h"
#include "common.h"
#include "class_drawpanel.h"

#include "pcbnew.h"
#include "wxPcbStruct.h"

/* Functions to undo and redo edit commands.
 *  commmands to undo are stored in CurrentScreen->m_UndoList
 *  commmands to redo are stored in CurrentScreen->m_RedoList
 *
 *  m_UndoList and m_RedoList handle a std::vector of PICKED_ITEMS_LIST
 *  Each PICKED_ITEMS_LIST handle a std::vector of pickers (class ITEM_PICKER),
 *  that store the list of schematic items that are concerned by the command to undo or redo
 *  and is created for each command to undo (handle also a command to redo).
 *  each picker has a pointer pointing to an item to undo or redo (in fact: deleted, added or modified),
 * and has a pointer to a copy of this item, when this item has been modified
 * (the old values of parameters are therefore saved)
 *
 *  there are 3 cases:
 *  - delete item(s) command
 *  - change item(s) command
 *  - add item(s) command
 *  and 3 cases for block:
 *  - move list of items
 *  - mirror (Y) list of items
 *  - Flip list of items
 *
 *  Undo command
 *  - delete item(s) command:
 *       =>  deleted items are moved in undo list
 *
 *  - change item(s) command
 *      => A copy of item(s) is made (a DrawPickedStruct list of wrappers)
 *      the .m_Link member of each wrapper points the modified item.
 *      the .m_Item member of each wrapper points the old copy of this item.
 *
 *  - add item(s) command
 *      =>A list of item(s) is made. The .m_Item member of each wrapper points the new item.
 *
 *  Redo command
 *  - delete item(s) old command:
 *      => deleted items are moved in EEDrawList list, and in
 *
 *  - change item(s) command
 *      => the copy of item(s) is moved in Undo list
 *
 *  - add item(s) command
 *      => The list of item(s) is used to create a deleted list in undo list(same as a delete command)
 *
 *   Some block operations that change items can be undoed without memorise items, just the coordiantes of the transform:
 *      move list of items (undo/redo is made by moving with the opposite move vector)
 *      mirror (Y) and flip list of items (undo/redo is made by mirror or flip items)
 *      so they are handled specifically.
 *
 */


/**************************************************************/
void SwapData( EDA_BaseStruct* aItem, EDA_BaseStruct* aImage )
/***************************************************************/

/* Used if undo / redo command:
 *  swap data between Item and its copy, pointed by its .m_Image member
 * swapped data is data modified by edition, so not all values are swapped
 */
{
    if( aItem == NULL || aImage == NULL )
    {
        wxMessageBox( wxT( "SwapData error: NULL pointer" ) );
        return;
    }

    switch( aItem->Type() )
    {
    default:
        wxMessageBox( wxT( "SwapData() error: unexpected type" ) );
        break;
    }
}


/************************************************************/
BOARD_ITEM* DuplicateStruct( BOARD_ITEM* aItem )
/************************************************************/

/* Routine to create a new copy of given struct.
 *  The new object is not put in list (not linked)
 */
{
    BOARD_ITEM* newItem = NULL;

    if( aItem == NULL )
    {
        wxMessageBox( wxT( "DuplicateStruct error: NULL struct" ) );
        return NULL;
    }

    switch( aItem->Type() )
    {
    default:
    {
        wxString msg;
        msg << wxT( "DuplicateStruct error: unexpected StructType " ) <<
        aItem->Type() << wxT( " " ) << aItem->GetClass();
//        wxMessageBox( msg );
    }
    break;
    }

    return newItem;
}


/***********************************************************************/
void WinEDA_PcbFrame::SaveCopyInUndoList( BOARD_ITEM*    aItemToCopy,
                                          UndoRedoOpType aCommandType,
                                          const wxPoint& aTransformPoint )
/***********************************************************************/

/** function SaveCopyInUndoList
 * Create a copy of the current schematic item, and put it in the undo list.
 *
 *  flag_type_command =
 *      UR_CHANGED
 *      UR_NEW
 *      UR_DELETED
 *
 *  If it is a delete command, items are put on list with the .Flags member set to UR_DELETED.
 *  When it will be really deleted, the EEDrawList and the subhierarchy will be deleted.
 *  If it is only a copy, the EEDrawList and the subhierarchy must NOT be deleted.
 *
 *  Note:
 *  Edit wires and busses is a bit complex.
 *  because when a new wire is added, modifications in wire list
 *  (wire concatenation) there are modified items, deleted items and new items
 *  so flag_type_command is UR_WIRE_IMAGE: the struct ItemToCopy is a list of wires
 *  saved in Undo List (for Undo or Redo commands, saved wires will be exchanged with current wire list
 */
{
    BOARD_ITEM*        CopyOfItem;
    PICKED_ITEMS_LIST* commandToUndo = new PICKED_ITEMS_LIST();

    commandToUndo->m_TransformPoint = aTransformPoint;

    ITEM_PICKER itemWrapper( aItemToCopy, aCommandType );

    switch( aCommandType )
    {
    case UR_CHANGED:            /* Create a copy of schematic */
        CopyOfItem = DuplicateStruct( aItemToCopy );
        itemWrapper.m_Item = CopyOfItem;
        itemWrapper.m_Link = aItemToCopy;
        if( CopyOfItem )
            commandToUndo->PushItem( itemWrapper );
        break;

    case UR_NEW:
    case UR_WIRE_IMAGE:
    case UR_DELETED:
        commandToUndo->PushItem( itemWrapper );
        break;

    default:
    {
        wxString msg;
        msg.Printf( wxT( "SaveCopyInUndoList() error (unknown code %X)" ), aCommandType );
        wxMessageBox( msg );
    }
    break;
    }

    if( commandToUndo->GetCount() )
    {
        /* Save the copy in undo list */
        GetScreen()->PushCommandToUndoList( commandToUndo );

        /* Clear redo list, because after new save there is no redo to do */
        GetScreen()->ClearUndoORRedoList( GetScreen()->m_RedoList );
    }
    else
        delete commandToUndo;
}


/** function SaveCopyInUndoList
 * @param aItemsList = a PICKED_ITEMS_LIST of items to save
 * @param aTypeCommand = type of comand ( UR_CHANGED, UR_NEW, UR_DELETED ...
 */
void WinEDA_PcbFrame::SaveCopyInUndoList( PICKED_ITEMS_LIST& aItemsList,
                                          UndoRedoOpType     aTypeCommand,
                                          const wxPoint&     aTransformPoint )
{
    BOARD_ITEM*        CopyOfItem;
    PICKED_ITEMS_LIST* commandToUndo = new PICKED_ITEMS_LIST();

    commandToUndo->m_TransformPoint = aTransformPoint;

    ITEM_PICKER itemWrapper;

    for( unsigned ii = 0; ii < aItemsList.GetCount(); ii++ )
    {
        BOARD_ITEM*    ItemToCopy = (BOARD_ITEM*) aItemsList.GetItemData( ii );
        UndoRedoOpType command    = aItemsList.GetItemStatus( ii );
        if( command == UR_UNSPECIFIED )
        {
            command = aTypeCommand;
        }
        wxASSERT( ItemToCopy );
        itemWrapper.m_Item = ItemToCopy;
        itemWrapper.m_UndoRedoStatus = command;
        switch( command )
        {
        case UR_CHANGED:        /* Create a copy of schematic */
            CopyOfItem = DuplicateStruct( ItemToCopy );
            itemWrapper.m_Item = CopyOfItem;
            itemWrapper.m_Link = ItemToCopy;
            if( CopyOfItem )
                commandToUndo->PushItem( itemWrapper );
            break;

        case UR_MOVED:
        case UR_MIRRORED_Y:
        case UR_NEW:
            commandToUndo->PushItem( itemWrapper );
            break;

        case UR_DELETED:
            ItemToCopy->m_Flags = UR_DELETED;
            commandToUndo->PushItem( itemWrapper );
            break;

        default:
        {
            wxString msg;
            msg.Printf( wxT( "SaveCopyInUndoList() error (unknown code %X)" ), command );
            wxMessageBox( msg );
        }
        break;
        }
    }

    if( commandToUndo->GetCount() )
    {
        /* Save the copy in undo list */
        GetScreen()->PushCommandToUndoList( commandToUndo );

        /* Clear redo list, because after new save there is no redo to do */
        GetScreen()->ClearUndoORRedoList( GetScreen()->m_RedoList );
    }
    else
        delete commandToUndo;
}


/***************************************************************************/
void WinEDA_PcbFrame::PutDataInPreviousState( PICKED_ITEMS_LIST* aList )
/***************************************************************************/

/* Used in undo or redo command.
 *  Put data pointed by List in the previous state, i.e. the state memorised by List
 */
{
    BOARD_ITEM* item;
    bool        as_moved = false;

    for( unsigned ii = 0; ii < aList->GetCount(); ii++  )
    {
        ITEM_PICKER itemWrapper = aList->GetItemWrapper( ii );
        item = (BOARD_ITEM*) itemWrapper.m_Item;
        wxASSERT( item );
        BOARD_ITEM* image = (BOARD_ITEM*) itemWrapper.m_Link;
        switch( itemWrapper.m_UndoRedoStatus )
        {
        case UR_CHANGED:    /* Exchange old and new data for each item */
            SwapData( item, image );
            break;

        case UR_NEW:        /* new items are deleted */
            aList->SetItemStatus( UR_DELETED, ii );
            GetBoard()->Remove( item );
            item->m_Flags = UR_DELETED;
            break;

        case UR_DELETED:    /* deleted items are put in List, as new items */
            aList->SetItemStatus( UR_NEW, ii );
            GetBoard()->Add( item );
            item->m_Flags = 0;
            break;

        case UR_MOVED:

            //           item->Move( - aList->m_TransformPoint );
            as_moved = true;
            break;

        case UR_MIRRORED_Y:

//            item->Mirror_Y( aList->m_TransformPoint.x );
            break;

        default:
        {
            wxString msg;
            msg.Printf( wxT(
                            "PutDataInPreviousState() error (unknown code %X)" ),
                        itemWrapper.m_UndoRedoStatus );
            wxMessageBox( msg );
        }
        break;
        }
    }

    // Undo for move transform needs to change the general move vector:
    if( as_moved )
        aList->m_TransformPoint = -aList->m_TransformPoint;

    Compile_Ratsnest( NULL, true );
}


/**********************************************************/
void WinEDA_PcbFrame::GetBoardFromUndoList( wxCommandEvent& event )
/**********************************************************/

/** Function GetSchematicFromUndoList
 *  Undo the last edition:
 *  - Save the current schematic in Redo list
 *  - Get an old version of the schematic
 *  @return false if nothing done, else true
 */
{
    if( GetScreen()->GetUndoCommandCount() <= 0 )
        return;

    /* Get the old wrapper and put it in RedoList */
    PICKED_ITEMS_LIST* List = GetScreen()->PopCommandFromUndoList();
    GetScreen()->PushCommandToRedoList( List );
    /* Undo the command */
    PutDataInPreviousState( List );

    GetScreen()->SetModify();
    ReCreateHToolbar();
    SetToolbars();

    DrawPanel->Refresh();
}


/**********************************************************/
void WinEDA_PcbFrame::GetBoardFromRedoList( wxCommandEvent& event )
/**********************************************************/

/* Redo the last edition:
 *  - Save the current schematic in undo list
 *  - Get the old version
 *  @return false if nothing done, else true
 */
{
    if( GetScreen()->GetRedoCommandCount() == 0 )
        return;


    /* Get the old wrapper and put it in UndoList */
    PICKED_ITEMS_LIST* List = GetScreen()->PopCommandFromRedoList();
    GetScreen()->PushCommandToUndoList( List );

    /* Redo the command: */
    PutDataInPreviousState( List );

    GetScreen()->SetModify();
    ReCreateHToolbar();
    SetToolbars();

    DrawPanel->Refresh();
}


/***********************************************************************************/
void PCB_SCREEN::ClearUndoORRedoList( UNDO_REDO_CONTAINER& aList, int aItemCount )
/**********************************************************************************/

/** Function ClearUndoORRedoList
 * free the undo or redo list from List element
 *  Wrappers are deleted.
 *  datas pointed by wrappers are deleted if not in use in schematic
 *  i.e. when they are copy of a schematic item or they are no more in use (DELETED)
 * @param aList = the UNDO_REDO_CONTAINER to clear
 * @param aItemCount = the count of items to remove. < 0 for all items
 * items (commands stored in list) are removed from the beginning of the list.
 * So this function can be called to remove old commands
 */
{
    if( aItemCount == 0 )
        return;

    unsigned icnt = aList.m_CommandsList.size();
    if( aItemCount > 0 )
        icnt = aItemCount;
    for( unsigned ii = 0; ii < icnt; ii++ )
    {
        if( aList.m_CommandsList.size() == 0 )
            break;
        PICKED_ITEMS_LIST* curr_cmd = aList.m_CommandsList[0];
        aList.m_CommandsList.erase( aList.m_CommandsList.begin() );

        // Delete items is they are not flagged UR_NEW, or if this is a block operation
        while( 1 )
        {
            ITEM_PICKER     wrapper = curr_cmd->PopItem();
            EDA_BaseStruct* item    = wrapper.m_Item;
            if( item == NULL ) // No more item in list.
                break;
            switch( wrapper.m_UndoRedoStatus )
            {
            case UR_MOVED:
            case UR_MIRRORED_X:
            case UR_MIRRORED_Y:
            case UR_ROTATED:
            case UR_NEW:        // Do nothing, items are in use, the picker is not owner of items
                break;

            default:
                delete item;    //  the picker is owner of this item
                break;
            }
        }

        delete curr_cmd;    // Delete command
    }
}
