/*
 * emulate the reader
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 */

#include "qemu-common.h"
#include "qemu-thread.h"

#include "vcard.h"
#include "vcard_emul.h"
#include "card_7816.h"
#include "vreader.h"
#include "vevent.h"

struct VReaderStruct {
    int    reference_count;
    VCard *card;
    char *name;
    vreader_id_t id;
    QemuMutex lock;
    VReaderEmul  *reader_private;
    VReaderEmulFree reader_private_free;
};

/* manage locking */
static inline void
vreader_lock(VReader *reader)
{
    qemu_mutex_lock(&reader->lock);
}

static inline void
vreader_unlock(VReader *reader)
{
    qemu_mutex_unlock(&reader->lock);
}

/*
 * vreader constructor
 */
VReader *
vreader_new(const char *name, VReaderEmul *private,
            VReaderEmulFree private_free)
{
    VReader *reader;

    reader = (VReader *)qemu_malloc(sizeof(VReader));
    qemu_mutex_init(&reader->lock);
    reader->reference_count = 1;
    reader->name = name ? strdup(name) : NULL;
    reader->card = NULL;
    reader->id = (vreader_id_t)-1;
    reader->reader_private = private;
    reader->reader_private_free = private_free;
    return reader;
}

/* get a reference */
VReader*
vreader_reference(VReader *reader)
{
    if (reader == NULL) {
        return NULL;
    }
    vreader_lock(reader);
    reader->reference_count++;
    vreader_unlock(reader);
    return reader;
}

/* free a reference */
void
vreader_free(VReader *reader)
{
    if (reader == NULL) {
        return;
    }
    vreader_lock(reader);
    if (reader->reference_count-- > 1) {
        vreader_unlock(reader);
        return;
    }
    vreader_unlock(reader);
    if (reader->card) {
        vcard_free(reader->card);
    }
    if (reader->name) {
        qemu_free(reader->name);
    }
    if (reader->reader_private_free) {
        reader->reader_private_free(reader->reader_private);
    }
    qemu_free(reader);
    return;
}

static VCard *
vreader_get_card(VReader *reader)
{
    VCard *card;

    vreader_lock(reader);
    card = vcard_reference(reader->card);
    vreader_unlock(reader);
    return card;
}

VReaderStatus
vreader_card_is_present(VReader *reader)
{
    VCard *card = vreader_get_card(reader);

    if (card == NULL) {
        return VREADER_NO_CARD;
    }
    vcard_free(card);
    return VREADER_OK;
}

vreader_id_t
vreader_get_id(VReader *reader)
{
    if (reader == NULL) {
        return (vreader_id_t)-1;
    }
    return reader->id;
}

VReaderStatus
vreader_set_id(VReader *reader, vreader_id_t id)
{
    if (reader == NULL) {
        return VREADER_NO_CARD;
    }
    reader->id = id;
    return VREADER_OK;
}

const char *
vreader_get_name(VReader *reader)
{
    if (reader == NULL) {
        return NULL;
    }
    return reader->name;
}

VReaderEmul *
vreader_get_private(VReader *reader)
{
    return reader->reader_private;
}

static VReaderStatus
vreader_reset(VReader *reader, VCardPower power, unsigned char *atr, int *len)
{
    VCard *card = vreader_get_card(reader);

    if (card == NULL) {
        return VREADER_NO_CARD;
    }
    /*
     * clean up our state
     */
    vcard_reset(card, power);
    if (atr) {
        vcard_get_atr(card, atr, len);
    }
    vcard_free(card); /* free our reference */
    return VREADER_OK;
}

VReaderStatus
vreader_power_on(VReader *reader, unsigned char *atr, int *len)
{
    return vreader_reset(reader, VCARD_POWER_ON, atr, len);
}

VReaderStatus
vreader_power_off(VReader *reader)
{
    return vreader_reset(reader, VCARD_POWER_OFF, NULL, 0);
}


VReaderStatus
vreader_xfr_bytes(VReader *reader,
                  unsigned char *send_buf, int send_buf_len,
                  unsigned char *receive_buf, int *receive_buf_len)
{
    VCardAPDU *apdu;
    VCardResponse *response = NULL;
    VCardStatus card_status;
    unsigned short status;
    VCard *card = vreader_get_card(reader);

    if (card == NULL) {
        return VREADER_NO_CARD;
    }

    apdu = vcard_apdu_new(send_buf, send_buf_len, &status);
    if (apdu == NULL) {
        response = vcard_make_response(status);
        card_status = VCARD_DONE;
    } else {
        card_status = vcard_process_apdu(card, apdu, &response);
    }
    assert(card_status == VCARD_DONE);
    if (card_status == VCARD_DONE) {
        int size = MIN(*receive_buf_len, response->b_total_len);
        memcpy(receive_buf, response->b_data, size);
        *receive_buf_len = size;
    }
    vcard_response_delete(response);
    vcard_apdu_delete(apdu);
    vcard_free(card); /* free our reference */
    return VREADER_OK;
}

struct VReaderListStruct {
    VReaderListEntry *head;
    VReaderListEntry *tail;
};

struct VReaderListEntryStruct {
    VReaderListEntry *next;
    VReaderListEntry *prev;
    VReader *reader;
};


static VReaderListEntry *
vreader_list_entry_new(VReader *reader)
{
    VReaderListEntry *new_reader_list_entry;

    new_reader_list_entry = (VReaderListEntry *)
                               qemu_malloc(sizeof(VReaderListEntry));
    new_reader_list_entry->next = NULL;
    new_reader_list_entry->prev = NULL;
    new_reader_list_entry->reader = vreader_reference(reader);
    return new_reader_list_entry;
}

static void
vreader_list_entry_delete(VReaderListEntry *entry)
{
    if (entry == NULL) {
        return;
    }
    vreader_free(entry->reader);
    qemu_free(entry);
}


static VReaderList *
vreader_list_new(void)
{
    VReaderList *new_reader_list;

    new_reader_list = (VReaderList *)qemu_malloc(sizeof(VReaderList));
    new_reader_list->head = NULL;
    new_reader_list->tail = NULL;
    return new_reader_list;
}

void
vreader_list_delete(VReaderList *list)
{
    VReaderListEntry *current_entry;
    VReaderListEntry *next_entry = NULL;
    for (current_entry = vreader_list_get_first(list); current_entry;
         current_entry = next_entry) {
        next_entry = vreader_list_get_next(current_entry);
        vreader_list_entry_delete(current_entry);
    }
    list->head = NULL;
    list->tail = NULL;
    qemu_free(list);
}


VReaderListEntry *
vreader_list_get_first(VReaderList *list)
{
    return list ? list->head : NULL;
}

VReaderListEntry *
vreader_list_get_next(VReaderListEntry *current)
{
    return current ? current->next : NULL;
}

VReader *
vreader_list_get_reader(VReaderListEntry *entry)
{
    return entry ? vreader_reference(entry->reader) : NULL;
}

static void
vreader_queue(VReaderList *list, VReaderListEntry *entry)
{
    if (entry == NULL) {
        return;
    }
    entry->next = NULL;
    entry->prev = list->tail;
    if (list->head) {
        list->tail->next = entry;
    } else {
        list->head = entry;
    }
    list->tail = entry;
}

static void
vreader_dequeue(VReaderList *list, VReaderListEntry *entry)
{
    if (entry == NULL) {
        return;
    }
    if (entry->next == NULL) {
        list->tail = entry->prev;
    } else if (entry->prev == NULL) {
        list->head = entry->next;
    } else {
        entry->prev->next = entry->next;
        entry->next->prev = entry->prev;
    }
    if ((list->tail == NULL) || (list->head == NULL)) {
        list->head = list->tail = NULL;
    }
    entry->next = entry->prev = NULL;
}

static VReaderList *vreader_list;
static QemuMutex vreader_list_mutex;

static void
vreader_list_init(void)
{
    vreader_list = vreader_list_new();
    qemu_mutex_init(&vreader_list_mutex);
}

static void
vreader_list_lock(void)
{
    qemu_mutex_lock(&vreader_list_mutex);
}

static void
vreader_list_unlock(void)
{
    qemu_mutex_unlock(&vreader_list_mutex);
}

static VReaderList *
vreader_copy_list(VReaderList *list)
{
    VReaderList *new_list = NULL;
    VReaderListEntry *current_entry = NULL;

    new_list = vreader_list_new();
    if (new_list == NULL) {
        return NULL;
    }
    for (current_entry = vreader_list_get_first(list); current_entry;
         current_entry = vreader_list_get_next(current_entry)) {
        VReader *reader = vreader_list_get_reader(current_entry);
        VReaderListEntry *new_entry = vreader_list_entry_new(reader);

        vreader_free(reader);
        vreader_queue(new_list, new_entry);
    }
    return new_list;
}

VReaderList *
vreader_get_reader_list(void)
{
    VReaderList *new_reader_list;

    vreader_list_lock();
    new_reader_list = vreader_copy_list(vreader_list);
    vreader_list_unlock();
    return new_reader_list;
}

VReader *
vreader_get_reader_by_id(vreader_id_t id)
{
    VReader *reader = NULL;
    VReaderListEntry *current_entry = NULL;

    if (id == (vreader_id_t) -1) {
        return NULL;
    }

    vreader_list_lock();
    for (current_entry = vreader_list_get_first(vreader_list); current_entry;
            current_entry = vreader_list_get_next(current_entry)) {
        VReader *creader = vreader_list_get_reader(current_entry);
        if (creader->id == id) {
            reader = creader;
            break;
        }
        vreader_free(creader);
    }
    vreader_list_unlock();
    return reader;
}

VReader *
vreader_get_reader_by_name(const char *name)
{
    VReader *reader = NULL;
    VReaderListEntry *current_entry = NULL;

    vreader_list_lock();
    for (current_entry = vreader_list_get_first(vreader_list); current_entry;
            current_entry = vreader_list_get_next(current_entry)) {
        VReader *creader = vreader_list_get_reader(current_entry);
        if (strcmp(creader->name, name) == 0) {
            reader = creader;
            break;
        }
        vreader_free(creader);
    }
    vreader_list_unlock();
    return reader;
}

/* called from card_emul to initialize the readers */
VReaderStatus
vreader_add_reader(VReader *reader)
{
    VReaderListEntry *reader_entry;

    reader_entry = vreader_list_entry_new(reader);
    if (reader_entry == NULL) {
        return VREADER_OUT_OF_MEMORY;
    }
    vreader_list_lock();
    vreader_queue(vreader_list, reader_entry);
    vreader_list_unlock();
    vevent_queue_vevent(vevent_new(VEVENT_READER_INSERT, reader, NULL));
    return VREADER_OK;
}


VReaderStatus
vreader_remove_reader(VReader *reader)
{
    VReaderListEntry *current_entry;

    vreader_list_lock();
    for (current_entry = vreader_list_get_first(vreader_list); current_entry;
         current_entry = vreader_list_get_next(current_entry)) {
        if (current_entry->reader == reader) {
            break;
        }
    }
    vreader_dequeue(vreader_list, current_entry);
    vreader_list_unlock();
    vreader_list_entry_delete(current_entry);
    vevent_queue_vevent(vevent_new(VEVENT_READER_REMOVE, reader, NULL));
    return VREADER_OK;
}

/*
 * Generate VEVENT_CARD_INSERT or VEVENT_CARD_REMOVE based on vreader
 * state. Separated from vreader_insert_card to allow replaying events
 * for a given state.
 */
void
vreader_queue_card_event(VReader *reader)
{
    vevent_queue_vevent(vevent_new(
        reader->card ? VEVENT_CARD_INSERT : VEVENT_CARD_REMOVE, reader,
        reader->card));
}

/*
 * insert/remove a new card. for removal, card == NULL
 */
VReaderStatus
vreader_insert_card(VReader *reader, VCard *card)
{
    vreader_lock(reader);
    if (reader->card) {
        /* decrement reference count */
        vcard_free(reader->card);
        reader->card = NULL;
    }
    reader->card = vcard_reference(card);
    vreader_unlock(reader);
    vreader_queue_card_event(reader);
    return VREADER_OK;
}

/*
 * initialize all the static reader structures
 */
void
vreader_init(void)
{
    vreader_list_init();
}

