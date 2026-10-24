// Contacts.xapp — Address Book for FreeBSD-X
//
// Layout:
//   Left panel (280px):  search bar + alphabetical contact list + "+" button
//   Right panel (flex):  contact detail view OR add/edit form
//
// Features:
//   • Alphabetical list (last name, first name)
//   • Search filters list in real time
//   • Detail view: full name, email, phone, org, notes + Edit / Delete
//   • Add / Edit form with Save / Cancel
//   • vCard (.vcf) drag-and-drop import via setOnDrop
//   • ContactChanged broadcast keeps list live
//   • MIME handler: "text/vcard" → opens .vcf via ImportVCard

#include "xkit/widget.h"
#include "xkit/application.h"
#include "xkit/xbus_client.h"
#include "shared/protocol.h"
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <algorithm>
#include <sstream>

using namespace fbx::xkit;
using namespace fbx::shared;

// ── Local contact model ───────────────────────────────────────────────────────

struct LocalContact {
    uint32_t    id{};
    std::string first_name;
    std::string last_name;
    std::string email;
    std::string phone;
    std::string organization;
    std::string notes;
};

static LocalContact fromEntry(const ContactEntry& e) {
    LocalContact c;
    c.id           = e.id_;
    c.first_name   = e.first_name_;
    c.last_name    = e.last_name_;
    c.email        = e.email_;
    c.phone        = e.phone_;
    c.organization = e.organization_;
    c.notes        = e.notes_;
    return c;
}

static void toEntry(ContactEntry& e, const LocalContact& c) {
    e.id_ = c.id;
    std::strncpy(e.first_name_,   c.first_name.c_str(),   sizeof(e.first_name_)   - 1);
    std::strncpy(e.last_name_,    c.last_name.c_str(),    sizeof(e.last_name_)    - 1);
    std::strncpy(e.email_,        c.email.c_str(),        sizeof(e.email_)        - 1);
    std::strncpy(e.phone_,        c.phone.c_str(),        sizeof(e.phone_)        - 1);
    std::strncpy(e.organization_, c.organization.c_str(), sizeof(e.organization_) - 1);
    std::strncpy(e.notes_,        c.notes.c_str(),        sizeof(e.notes_)        - 1);
}

// ── App ───────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // Check for --open <path> to import a vCard on launch
    std::string open_path;
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--open") {
            open_path = argv[i + 1];
            break;
        }
    }

    // ── Shared state ─────────────────────────────────────────────────────────
    auto contacts     = std::make_shared<std::vector<LocalContact>>();
    auto selected_id  = std::make_shared<uint32_t>(0);
    auto search_query = std::make_shared<std::string>("");
    auto editing      = std::make_shared<bool>(false); // true = add/edit form open
    auto edit_id      = std::make_shared<uint32_t>(0); // 0 = new contact

    // Forward declarations (populated below)
    auto list_col  = std::make_shared<std::shared_ptr<Widget>>();
    auto detail_col= std::make_shared<std::shared_ptr<Widget>>();
    auto right_pane= std::make_shared<std::shared_ptr<Widget>>();

    // ── Edit form fields (shared_ptr to allow lambda capture) ────────────────
    auto ef_first = std::make_shared<std::shared_ptr<Widget>>();
    auto ef_last  = std::make_shared<std::shared_ptr<Widget>>();
    auto ef_email = std::make_shared<std::shared_ptr<Widget>>();
    auto ef_phone = std::make_shared<std::shared_ptr<Widget>>();
    auto ef_org   = std::make_shared<std::shared_ptr<Widget>>();
    auto ef_notes = std::make_shared<std::shared_ptr<Widget>>();

    // Rebuild lambdas — forward-declared
    auto rebuild_list   = std::make_shared<std::function<void()>>();
    auto show_detail    = std::make_shared<std::function<void(uint32_t)>>();
    auto show_edit_form = std::make_shared<std::function<void(uint32_t)>>();

    // ── Root layout: flex row ─────────────────────────────────────────
    auto root_row = std::make_shared<FlexRow>();
    root_row->setFixedSize(820, 600);

    // ══ LEFT PANEL ═══════════════════════════════════════════════════
    auto left = std::make_shared<FlexColumn>();
    left->setFixedSize(280, 0.0f);
    left->setPadding(12);
    left->setGap(8);
    left->setBackgroundColor(Color{0.13f, 0.13f, 0.13f, 1.0f});
    root_row->addChild(left);

    // Search bar
    auto search_input = std::make_shared<TextInputWidget>("");
    search_input->setPlaceholder("Search contacts…");
    search_input->setFixedSize(0.0f, 32);
    search_input->setOnChanged([search_query, rebuild_list](const std::string& q) {
        *search_query = q;
        if (*rebuild_list) (*rebuild_list)();
    });
    left->addChild(search_input);

    // Contact list scroll
    auto list_column = std::make_shared<FlexColumn>();
    list_column->setGap(2);
    *list_col = list_column;

    auto list_scroll = std::make_shared<ScrollViewWidget>(list_column);
    list_scroll->setFlexGrow(1.0f);
    left->addChild(list_scroll);

    // "+" Add contact button
    auto add_btn = std::make_shared<ButtonWidget>("+ Add Contact");
    add_btn->setFixedSize(0.0f, 36);
    add_btn->setBackgroundColor(Color{0.20f, 0.50f, 0.90f, 1.0f});
    add_btn->onClick([show_edit_form]() {
        if (*show_edit_form) (*show_edit_form)(0);
    });
    left->addChild(add_btn);

    // ══ RIGHT PANEL ═══════════════════════════════════════════════════
    auto right = std::make_shared<FlexColumn>();
    right->setFlexGrow(1.0f);
    right->setBackgroundColor(Color{0.10f, 0.10f, 0.10f, 1.0f});
    root_row->addChild(right);
    *right_pane = right;

    // ── Empty-state placeholder ───────────────────────────────────────
    auto empty_label = std::make_shared<TextWidget>("Select a contact", 16.0f);
    empty_label->setColor(0.5f, 0.5f, 0.5f, 1.0f);
    empty_label->setPadding(40);
    right->addChild(empty_label);

    // ══ REBUILD LIST LAMBDA ════════════════════════════════════════════
    *rebuild_list = [=]() {
        auto lc = std::static_pointer_cast<FlexColumn>(*list_col);
        if (!lc) return;
        lc->clearChildren();

        // Filter contacts
        std::string ql = *search_query;
        auto toLower = [](std::string s) {
            for (auto& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        std::string qll = toLower(ql);

        // Sort alphabetically
        std::vector<LocalContact> sorted = *contacts;
        std::sort(sorted.begin(), sorted.end(), [](const LocalContact& a, const LocalContact& b) {
            if (a.last_name != b.last_name) return a.last_name < b.last_name;
            return a.first_name < b.first_name;
        });

        for (const auto& c : sorted) {
            if (!qll.empty()) {
                bool match =
                    toLower(c.first_name).find(qll)   != std::string::npos ||
                    toLower(c.last_name).find(qll)    != std::string::npos ||
                    toLower(c.email).find(qll)        != std::string::npos ||
                    toLower(c.phone).find(qll)        != std::string::npos ||
                    toLower(c.organization).find(qll) != std::string::npos;
                if (!match) continue;
            }

            std::string display_name = c.first_name + " " + c.last_name;
            if (display_name == " ") display_name = c.email;
            auto row = std::make_shared<ButtonWidget>(display_name);
            row->setFixedSize(0.0f, 44);

            uint32_t cid = c.id;
            bool is_selected = (cid == *selected_id);
            if (is_selected)
                row->setBackgroundColor(Color{0.20f, 0.50f, 0.90f, 0.25f});
            else
                row->setBackgroundColor(Color{0.0f, 0.0f, 0.0f, 0.0f});

            row->onClick([cid, show_detail]() {
                if (*show_detail) (*show_detail)(cid);
            });
            lc->addChild(row);
        }

        if (sorted.empty() || (contacts->empty())) {
            auto hint = std::make_shared<TextWidget>("No contacts", 13.0f);
            hint->setColor(0.4f, 0.4f, 0.4f, 1.0f);
            hint->setPadding(12);
            lc->addChild(hint);
        }
    };

    // ══ SHOW DETAIL LAMBDA ════════════════════════════════════════════
    *show_detail = [=](uint32_t cid) {
        *selected_id = cid;
        *editing = false;

        auto rp = std::static_pointer_cast<FlexColumn>(*right_pane);
        if (!rp) return;
        rp->clearChildren();

        // Find contact
        LocalContact* found = nullptr;
        for (auto& c : *contacts) {
            if (c.id == cid) { found = &c; break; }
        }
        if (!found) {
            auto lbl = std::make_shared<TextWidget>("Contact not found", 14.0f);
            lbl->setColor(0.5f,0.5f,0.5f,1.0f);
            lbl->setPadding(40);
            rp->addChild(lbl);
            (*rebuild_list)();
            return;
        }

        const auto& c = *found;

        // Header: full name + action buttons
        auto hdr = std::make_shared<FlexRow>();
        hdr->setPadding(20);
        hdr->setGap(8);
        rp->addChild(hdr);

        auto name_lbl = std::make_shared<TextWidget>(c.first_name + " " + c.last_name, 22.0f);
        name_lbl->setColor(1.0f,1.0f,1.0f,1.0f);
        name_lbl->setFlexGrow(1.0f);
        hdr->addChild(name_lbl);

        auto edit_btn = std::make_shared<ButtonWidget>("Edit");
        edit_btn->setFixedSize(60, 30);
        uint32_t edit_cid = cid;
        edit_btn->onClick([edit_cid, show_edit_form]() {
            if (*show_edit_form) (*show_edit_form)(edit_cid);
        });
        hdr->addChild(edit_btn);

        auto del_btn = std::make_shared<ButtonWidget>("Delete");
        del_btn->setFixedSize(70, 30);
        del_btn->setBackgroundColor(Color{0.75f, 0.15f, 0.15f, 1.0f});
        del_btn->onClick([cid, contacts, selected_id, rebuild_list, right_pane]() {
            ContactsIdRequest req{};
            req.id_ = cid;
            auto* xbus = XApplication::getBus();
            if (!xbus) return;
            xbus->Call("com.freebsdx.Contacts",
                       static_cast<uint32_t>(ContactsOpcode::Delete),
                       {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                       [contacts, cid, selected_id, rebuild_list, right_pane](const XBusMessage&) {
                           contacts->erase(std::remove_if(contacts->begin(), contacts->end(),
                               [cid](const LocalContact& c){ return c.id == cid; }),
                               contacts->end());
                           *selected_id = 0;
                           auto rp2 = std::static_pointer_cast<FlexColumn>(*right_pane);
                           if (rp2) {
                               rp2->clearChildren();
                               auto lbl = std::make_shared<TextWidget>("Select a contact", 16.0f);
                               lbl->setColor(0.5f,0.5f,0.5f,1.0f);
                               lbl->setPadding(40);
                               rp2->addChild(lbl);
                           }
                           if (*rebuild_list) (*rebuild_list)();
                       });
        });
        hdr->addChild(del_btn);

        rp->addChild(std::make_shared<DividerWidget>());

        // Fields
        auto fields_col = std::make_shared<FlexColumn>();
        fields_col->setPadding(20);
        fields_col->setGap(16);
        rp->addChild(fields_col);

        auto make_field = [&](const std::string& label, const std::string& value) {
            if (value.empty()) return;
            auto row = std::make_shared<FlexColumn>();
            row->setGap(4);
            auto lbl = std::make_shared<TextWidget>(label, 11.0f);
            lbl->setColor(0.5f, 0.5f, 0.5f, 1.0f);
            row->addChild(lbl);
            auto val = std::make_shared<TextWidget>(value, 15.0f);
            val->setColor(0.9f, 0.9f, 0.9f, 1.0f);
            row->addChild(val);
            fields_col->addChild(row);
        };

        make_field("EMAIL",        c.email);
        make_field("PHONE",        c.phone);
        make_field("ORGANIZATION", c.organization);
        make_field("NOTES",        c.notes);

        (*rebuild_list)();
    };

    // ══ SHOW EDIT FORM LAMBDA ════════════════════════════════════════
    *show_edit_form = [=](uint32_t cid) {
        *edit_id = cid;
        *editing = true;
        *selected_id = cid;

        auto rp = std::static_pointer_cast<FlexColumn>(*right_pane);
        if (!rp) return;
        rp->clearChildren();

        // Find existing contact (if editing)
        LocalContact existing;
        if (cid != 0) {
            for (const auto& c : *contacts)
                if (c.id == cid) { existing = c; break; }
        }

        // Title
        auto title_lbl = std::make_shared<TextWidget>(cid == 0 ? "New Contact" : "Edit Contact", 18.0f);
        title_lbl->setColor(1.0f, 1.0f, 1.0f, 1.0f);
        title_lbl->setPadding(20);
        rp->addChild(title_lbl);
        rp->addChild(std::make_shared<DividerWidget>());

        // Form fields
        auto form = std::make_shared<FlexColumn>();
        form->setPadding(20);
        form->setGap(12);
        rp->addChild(form);

        auto make_input = [&](const std::string& label,
                              const std::string& placeholder,
                              const std::string& initial,
                              std::shared_ptr<std::shared_ptr<Widget>> out_ptr) {
            auto row = std::make_shared<FlexColumn>();
            row->setGap(4);
            auto lbl = std::make_shared<TextWidget>(label, 11.0f);
            lbl->setColor(0.5f, 0.5f, 0.5f, 1.0f);
            row->addChild(lbl);
            auto inp = std::make_shared<TextInputWidget>(initial);
            inp->setPlaceholder(placeholder);
            inp->setFixedSize(0.0f, 32);
            *out_ptr = inp;
            row->addChild(inp);
            form->addChild(row);
        };

        make_input("FIRST NAME", "First name", existing.first_name, ef_first);
        make_input("LAST NAME",  "Last name",  existing.last_name,  ef_last);
        make_input("EMAIL",      "Email address", existing.email,   ef_email);
        make_input("PHONE",      "Phone number",  existing.phone,   ef_phone);
        make_input("ORGANIZATION","Organization",  existing.organization, ef_org);
        make_input("NOTES",      "Notes",         existing.notes,   ef_notes);

        // Buttons row
        auto btn_row = std::make_shared<FlexRow>();
        btn_row->setPadding(20);
        btn_row->setGap(12);
        rp->addChild(btn_row);

        auto save_btn = std::make_shared<ButtonWidget>("Save", [=]() {
            LocalContact nc;
            nc.id           = *edit_id;
            auto get_text = [](std::shared_ptr<std::shared_ptr<Widget>> p) -> std::string {
                if (!p || !*p) return {};
                auto* inp = dynamic_cast<TextInputWidget*>((*p).get());
                return inp ? inp->getText() : std::string{};
            };
            nc.first_name   = get_text(ef_first);
            nc.last_name    = get_text(ef_last);
            nc.email        = get_text(ef_email);
            nc.phone        = get_text(ef_phone);
            nc.organization = get_text(ef_org);
            nc.notes        = get_text(ef_notes);

            ContactEntry entry{};
            toEntry(entry, nc);

            auto opcode = (*edit_id == 0)
                ? ContactsOpcode::Add
                : ContactsOpcode::Update;

            auto* xbus = XApplication::getBus();
            if (!xbus) return;
            xbus->Call("com.freebsdx.Contacts",
                       static_cast<uint32_t>(opcode),
                       {reinterpret_cast<const uint8_t*>(&entry), sizeof(entry)},
                       [contacts, selected_id, show_detail, rebuild_list](const XBusMessage& ack) {
                           ContactEntry r{};
                           std::memcpy(&r, ack.payload_, sizeof(r));
                           if (r.id_ == 0) return;
                           LocalContact saved = fromEntry(r);
                           bool found = false;
                           for (auto& c : *contacts) {
                               if (c.id == saved.id) { c = saved; found = true; break; }
                           }
                           if (!found) contacts->push_back(saved);
                           *selected_id = saved.id;
                           if (*rebuild_list) (*rebuild_list)();
                           if (*show_detail)  (*show_detail)(saved.id);
                       });
        });
        save_btn->setFixedSize(80, 36);
        save_btn->setBackgroundColor(Color{0.20f, 0.50f, 0.90f, 1.0f});
        btn_row->addChild(save_btn);

        auto cancel_btn = std::make_shared<ButtonWidget>("Cancel", [=]() {
            uint32_t prev = *selected_id;
            if (prev != 0 && *show_detail)
                (*show_detail)(prev);
            else {
                auto rp2 = std::static_pointer_cast<FlexColumn>(*right_pane);
                if (rp2) {
                    rp2->clearChildren();
                    auto lbl = std::make_shared<TextWidget>("Select a contact", 16.0f);
                    lbl->setColor(0.5f,0.5f,0.5f,1.0f);
                    lbl->setPadding(40);
                    rp2->addChild(lbl);
                }
            }
        });
        cancel_btn->setFixedSize(80, 36);
        btn_row->addChild(cancel_btn);
    };

    return XApplication::Run(root_row, "/tmp/freebsdx.sock", false, false,
        [=](XBusClient& xbus) {
            XApplication::setTitle("Contacts");

            // Paged loader for all contacts
            auto load_page = std::make_shared<std::function<void(uint32_t)>>();
            *load_page = [=](uint32_t page) {
                auto* xbus2 = XApplication::getBus();
                if (!xbus2) return;
                ContactsPageRequest req{};
                req.page_ = page;
                xbus2->Call("com.freebsdx.Contacts",
                            static_cast<uint32_t>(ContactsOpcode::List),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                            [page, contacts, rebuild_list, load_page](const XBusMessage& ack) {
                                ContactsListPage pg{};
                                std::memcpy(&pg, ack.payload_, sizeof(pg));
                                if (pg.valid_) {
                                    LocalContact c = fromEntry(pg.entry_);
                                    bool exists = false;
                                    for (auto& ec : *contacts)
                                        if (ec.id == c.id) { exists = true; break; }
                                    if (!exists) contacts->push_back(c);
                                }
                                if (pg.has_more_ && *load_page)
                                    (*load_page)(page + 1);
                                else if (*rebuild_list)
                                    (*rebuild_list)();
                            });
            };
            (*load_page)(0);

            // Broadcast handler
            xbus.OnBroadcast([=](const XBusMessage& msg) {
                if (std::strcmp(msg.service_name_, "com.freebsdx.Contacts") != 0) return;
                if (msg.opcode_ != static_cast<uint32_t>(ContactsOpcode::ContactChanged)) return;
                contacts->clear();
                auto* xbus2 = XApplication::getBus();
                if (!xbus2) return;
                ContactsPageRequest req{};
                req.page_ = 0;
                xbus2->Call("com.freebsdx.Contacts",
                            static_cast<uint32_t>(ContactsOpcode::List),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                            [contacts, rebuild_list](const XBusMessage& ack) {
                                ContactsListPage pg{};
                                std::memcpy(&pg, ack.payload_, sizeof(pg));
                                if (pg.valid_) contacts->push_back(fromEntry(pg.entry_));
                                if (*rebuild_list) (*rebuild_list)();
                            });
            });

            // Drag and drop (.vcf import)
            XApplication::setOnDrop("text/uri-list",
                [](float /*x*/, float /*y*/, const std::string& uri) {
                    std::string path = uri;
                    if (path.rfind("file://", 0) == 0) path = path.substr(7);
                    if (path.size() < 4 ||
                        path.substr(path.size() - 4) != ".vcf") return;
                    ContactsImportRequest req{};
                    std::strncpy(req.path_, path.c_str(), sizeof(req.path_) - 1);
                    auto* xbus2 = XApplication::getBus();
                    if (!xbus2) return;
                    xbus2->Call("com.freebsdx.Contacts",
                                static_cast<uint32_t>(ContactsOpcode::ImportVCard),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                [](const XBusMessage&) {});
                });

            // --open <path> import on launch
            if (!open_path.empty()) {
                ContactsImportRequest req{};
                std::strncpy(req.path_, open_path.c_str(), sizeof(req.path_) - 1);
                xbus.Call("com.freebsdx.Contacts",
                          static_cast<uint32_t>(ContactsOpcode::ImportVCard),
                          {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                          [](const XBusMessage&) {});
            }
        });
}
