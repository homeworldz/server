#include "homeworldz/viewer_protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>

namespace {
using namespace homeworldz::viewer;
using namespace std::chrono_literals;

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

void write_f32(std::vector<std::byte>& output, std::size_t offset, float value) {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    for (std::size_t index = 0; index < 4; ++index)
        output[offset + index] = static_cast<std::byte>(bits >> (index * 8));
}

void write_u32(std::vector<std::byte>& output, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index)
        output[offset + index] = static_cast<std::byte>(value >> (index * 8));
}

bool packet_round_trip() {
    Packet packet;
    packet.flags = flag_zero_coded | flag_reliable;
    packet.sequence = 0x10203040;
    packet.extra_header = bytes({9, 8});
    packet.payload = bytes({1, 0, 0, 0, 2, 0, 3});
    packet.acknowledgements = {4, 0x55667788};
    const auto encoded = encode_packet(packet);
    const auto decoded = decode_packet(encoded);
    return decoded && decoded->sequence == packet.sequence && decoded->extra_header == packet.extra_header &&
           decoded->payload == packet.payload && decoded->acknowledgements == packet.acknowledgements &&
           (decoded->flags & flag_appended_acks);
}

bool reliability() {
    const auto start = Circuit::Clock::time_point{};
    Circuit sender(start);
    Circuit receiver(start);
    const auto first = sender.send(bytes({1, 2, 3}), true, start);
    if (!first || sender.pending_reliable() != 1) return false;
    const auto received = receiver.receive(*first, start + 1ms);
    if (!received || received->payload != bytes({1, 2, 3})) return false;
    if (receiver.receive(*first, start + 2ms)) return false;
    const auto acknowledgements = receiver.poll(start + 3ms);
    if (acknowledgements.size() != 1) return false;
    const auto acknowledgement_packet = decode_packet(acknowledgements.front());
    if (!acknowledgement_packet) return false;
    const auto acknowledged = decode_packet_ack(acknowledgement_packet->payload);
    if (!acknowledged || *acknowledged != std::vector<std::uint32_t>{1}) return false;
    if (!sender.receive(acknowledgements.front(), start + 4ms) || sender.pending_reliable() != 0) return false;
    return true;
}

bool task_inventory_codecs() {
    const auto agent = *parse_uuid("12345678-1234-4234-8234-123456789abc");
    const auto session = *parse_uuid("87654321-4321-4321-8321-cba987654321");
    const auto task = *parse_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    auto request = bytes({0xff, 0xff, 0x01, 0x21});
    request.insert(request.end(), agent.begin(), agent.end());
    request.insert(request.end(), session.begin(), session.end());
    request.insert(request.end(), {std::byte{0x78}, std::byte{0x56},
                                   std::byte{0x34}, std::byte{0x12}});
    const auto decoded = decode_request_task_inventory(request);
    if (!decoded || decoded->agent_id != agent || decoded->session_id != session ||
        decoded->local_id != 0x12345678) return false;
    request.pop_back();
    if (decode_request_task_inventory(request)) return false;

    const auto reply = encode_reply_task_inventory({task, 0x1234, "inventory_1.tmp"});
    if (reply.size() != 4 + 16 + 2 + 1 + 15 || reply[0] != std::byte{0xff} ||
        reply[1] != std::byte{0xff} || reply[2] != std::byte{0x01} ||
        reply[3] != std::byte{0x22} ||
        !std::equal(task.begin(), task.end(), reply.begin() + 4) ||
        reply[20] != std::byte{0x34} || reply[21] != std::byte{0x12} ||
        reply[22] != std::byte{15}) return false;
    const auto empty = encode_reply_task_inventory({task, 0, {}});
    if (empty.size() != 23 || empty.back() != std::byte{}) return false;

    std::vector<std::byte> update(188);
    update[0] = std::byte{0xff};
    update[1] = std::byte{0xff};
    update[2] = std::byte{0x01};
    update[3] = std::byte{0x1e};
    std::copy(agent.begin(), agent.end(), update.begin() + 4);
    std::copy(session.begin(), session.end(), update.begin() + 20);
    update[36] = std::byte{42};
    update[40] = std::byte{1};
    std::copy(task.begin(), task.end(), update.begin() + 41);
    std::copy(session.begin(), session.end(), update.begin() + 57);
    std::copy(agent.begin(), agent.end(), update.begin() + 73);
    std::copy(agent.begin(), agent.end(), update.begin() + 89);
    std::copy(session.begin(), session.end(), update.begin() + 105);
    write_u32(update, 121, 0x0008e000);
    write_u32(update, 125, 0x0008a000);
    write_u32(update, 129, 0x00008000);
    write_u32(update, 133, 0x00002000);
    write_u32(update, 137, 0x0000a000);
    std::copy(session.begin(), session.end(), update.begin() + 142);
    update[158] = std::byte{0};
    update[159] = std::byte{0};
    write_u32(update, 160, 0x01020304);
    update[164] = std::byte{1};
    write_u32(update, 165, 25);
    update[169] = std::byte{8};
    std::copy_n(reinterpret_cast<const std::byte*>("Texture\0"), 8, update.begin() + 170);
    update[178] = std::byte{1};
    write_u32(update, 180, 1234567890);
    write_u32(update, 184, 0x10203040);
    const auto decoded_update = decode_update_task_inventory(update);
    if (!decoded_update || decoded_update->agent_id != agent ||
        decoded_update->session_id != session || decoded_update->local_id != 42 ||
        decoded_update->key != 1 || decoded_update->item_id != task ||
        decoded_update->folder_id != session || decoded_update->creator_id != agent ||
        decoded_update->owner_id != agent || decoded_update->group_id != session ||
        decoded_update->base_permissions != 0x0008e000 ||
        decoded_update->owner_permissions != 0x0008a000 ||
        decoded_update->group_permissions != 0x00008000 ||
        decoded_update->everyone_permissions != 0x00002000 ||
        decoded_update->next_owner_permissions != 0x0000a000 ||
        decoded_update->transaction_id != session || decoded_update->asset_type != 0 ||
        decoded_update->inventory_type != 0 || decoded_update->flags != 0x01020304 ||
        decoded_update->sale_type != 1 || decoded_update->sale_price != 25 ||
        decoded_update->name != "Texture" || !decoded_update->description.empty() ||
        decoded_update->creation_date != 1234567890 || decoded_update->crc != 0x10203040)
        return false;
    update.pop_back();
    if (decode_update_task_inventory(update)) return false;

    std::vector<std::byte> rez_script(218);
    rez_script[0] = std::byte{0xff};
    rez_script[1] = std::byte{0xff};
    rez_script[2] = std::byte{0x01};
    rez_script[3] = std::byte{0x30};
    std::copy(agent.begin(), agent.end(), rez_script.begin() + 4);
    std::copy(session.begin(), session.end(), rez_script.begin() + 20);
    std::copy(session.begin(), session.end(), rez_script.begin() + 36);
    write_u32(rez_script, 52, 42);
    rez_script[56] = std::byte{1};
    std::copy(task.begin(), task.end(), rez_script.begin() + 57);
    std::copy(agent.begin(), agent.end(), rez_script.begin() + 89);
    std::copy(agent.begin(), agent.end(), rez_script.begin() + 105);
    write_u32(rez_script, 137, 0x0008e000);
    write_u32(rez_script, 141, 0x0008a000);
    write_u32(rez_script, 145, 0x00008000);
    write_u32(rez_script, 149, 0x00002000);
    write_u32(rez_script, 153, 0x0000a000);
    rez_script[174] = std::byte{10};
    rez_script[175] = std::byte{10};
    rez_script[185] = std::byte{11};
    std::copy_n(reinterpret_cast<const std::byte*>("New Script\0"), 11,
                rez_script.begin() + 186);
    rez_script[197] = std::byte{12};
    std::copy_n(reinterpret_cast<const std::byte*>("lsl2 script\0"), 12,
                rez_script.begin() + 198);
    write_u32(rez_script, 210, 1234567890);
    write_u32(rez_script, 214, 0x10203040);
    const auto decoded_rez = decode_rez_script(rez_script);
    if (!decoded_rez || decoded_rez->agent_id != agent ||
        decoded_rez->session_id != session || decoded_rez->agent_group_id != session ||
        decoded_rez->local_id != 42 || !decoded_rez->enabled ||
        decoded_rez->item_id != task || decoded_rez->creator_id != agent ||
        decoded_rez->owner_id != agent || decoded_rez->base_permissions != 0x0008e000 ||
        decoded_rez->owner_permissions != 0x0008a000 ||
        decoded_rez->group_permissions != 0x00008000 ||
        decoded_rez->everyone_permissions != 0x00002000 ||
        decoded_rez->next_owner_permissions != 0x0000a000 ||
        decoded_rez->asset_type != 10 || decoded_rez->inventory_type != 10 ||
        decoded_rez->name != "New Script" || decoded_rez->description != "lsl2 script" ||
        decoded_rez->creation_date != 1234567890 || decoded_rez->crc != 0x10203040)
        return false;
    rez_script.pop_back();
    if (decode_rez_script(rez_script)) return false;

    auto remove = bytes({0xff, 0xff, 0x01, 0x1f});
    remove.insert(remove.end(), agent.begin(), agent.end());
    remove.insert(remove.end(), session.begin(), session.end());
    remove.insert(remove.end(), {std::byte{42}, std::byte{}, std::byte{}, std::byte{}});
    remove.insert(remove.end(), task.begin(), task.end());
    const auto decoded_remove = decode_remove_task_inventory(remove);
    if (!decoded_remove || decoded_remove->agent_id != agent ||
        decoded_remove->session_id != session || decoded_remove->local_id != 42 ||
        decoded_remove->item_id != task) return false;

    auto move = bytes({0xff, 0xff, 0x01, 0x20});
    move.insert(move.end(), agent.begin(), agent.end());
    move.insert(move.end(), session.begin(), session.end());
    move.insert(move.end(), task.begin(), task.end());
    move.insert(move.end(), {std::byte{42}, std::byte{}, std::byte{}, std::byte{}});
    move.insert(move.end(), session.begin(), session.end());
    const auto decoded_move = decode_move_task_inventory(move);
    if (!decoded_move || decoded_move->agent_id != agent ||
        decoded_move->session_id != session || decoded_move->folder_id != task ||
        decoded_move->local_id != 42 || decoded_move->item_id != session)
        return false;
    move.pop_back();
    if (decode_move_task_inventory(move)) return false;

    auto request_xfer = bytes({0xff, 0xff, 0x00, 0x9c,
                               0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
                               16});
    for (const char value : std::string("inventory_1.tmp\0", 16))
        request_xfer.push_back(static_cast<std::byte>(value));
    request_xfer.insert(request_xfer.end(), 21, std::byte{});
    const auto decoded_xfer = decode_request_xfer(request_xfer);
    const auto xfer_payload = encode_send_xfer_packet(
        0x0102030405060708ULL, 0x80000000U, bytes({1, 2, 3}));
    const auto confirmation_payload = encode_confirm_xfer_packet(0x0102030405060708ULL, 3);
    const auto decoded_confirmation = decode_confirm_xfer_packet(confirmation_payload);
    return decoded_xfer && decoded_xfer->id == 0x0102030405060708ULL &&
        decoded_xfer->filename == "inventory_1.tmp" && xfer_payload.size() == 18 &&
        xfer_payload[0] == std::byte{18} && xfer_payload[9] == std::byte{} &&
        xfer_payload[12] == std::byte{0x80} && xfer_payload[13] == std::byte{3} &&
        decoded_confirmation && decoded_confirmation->id == 0x0102030405060708ULL &&
        decoded_confirmation->packet == 3;
}

bool message_codecs() {
    UseCircuitCode expected;
    expected.circuit_code = 0x10203040;
    for (std::size_t index = 0; index < expected.session_id.size(); ++index) {
        expected.session_id[index] = static_cast<std::byte>(index);
        expected.agent_id[index] = static_cast<std::byte>(index + 16);
    }
    const auto payload = encode_use_circuit_code(expected);
    const auto decoded = decode_use_circuit_code(payload);
    if (!decoded || decoded->circuit_code != expected.circuit_code || decoded->session_id != expected.session_id ||
        decoded->agent_id != expected.agent_id)
        return false;
    if (payload.size() != 40 || payload[0] != std::byte{0xff} || payload[1] != std::byte{0xff} ||
        payload[2] != std::byte{0} || payload[3] != std::byte{3} || payload[4] != std::byte{0x40} ||
        payload[5] != std::byte{0x30} || payload[6] != std::byte{0x20} || payload[7] != std::byte{0x10})
        return false;
    const std::array<std::uint32_t, 2> sequences{0x01020304, 0xa0b0c0d0};
    const auto ack = encode_packet_ack(sequences);
    if (ack.size() != 13 || ack[5] != std::byte{4} || ack[6] != std::byte{3} ||
        ack[7] != std::byte{2} || ack[8] != std::byte{1}) return false;
    const auto ack_decoded = decode_packet_ack(ack);
    if (!ack_decoded || *ack_decoded != std::vector<std::uint32_t>(sequences.begin(), sequences.end())) return false;

    auto reply = bytes({0xff, 0xff, 0x00, 0x95});
    reply.insert(reply.end(), expected.agent_id.begin(), expected.agent_id.end());
    reply.insert(reply.end(), expected.session_id.begin(), expected.session_id.end());
    reply.insert(reply.end(), {std::byte{7}, std::byte{}, std::byte{}, std::byte{}});
    const auto handshake_reply = decode_region_handshake_reply(reply);
    if (!handshake_reply || handshake_reply->agent_id != expected.agent_id ||
        handshake_reply->session_id != expected.session_id) return false;
    reply[3] = std::byte{0xf9};
    const auto movement = decode_complete_agent_movement(reply);
    if (!movement || movement->circuit_code != 7 || movement->agent_id != expected.agent_id) return false;

    RegionHandshake handshake{"Test Region", expected.agent_id, expected.session_id, 21.5F};
    const auto encoded_handshake = encode_region_handshake(handshake);
    if (encoded_handshake.size() < 250 || encoded_handshake[3] != std::byte{0x94}) return false;
    // The four terrain numbers are per region, so the handshake must carry what
    // the caller set rather than the shipped defaults. Start heights then ranges. Asserted by finding
    // the eight floats as one contiguous block, low corners then high corners,
    // which is offset-independent and still catches a swap or a stride error.
    // Deliberately asymmetric: uniform values pass a codec that writes one
    // number four times.
    handshake.terrain_start = {20.0F, 21.5F, 22.0F, 23.25F};
    handshake.terrain_range = {60.0F, 61.5F, 62.0F, 63.25F};
    const auto encoded_elevations = encode_region_handshake(handshake);
    std::vector<std::byte> expected_block;
    for (const float value : {20.0F, 21.5F, 22.0F, 23.25F, 60.0F, 61.5F, 62.0F, 63.25F}) {
        std::array<std::byte, 4> raw{};
        std::memcpy(raw.data(), &value, sizeof(value));
        expected_block.insert(expected_block.end(), raw.begin(), raw.end());
    }
    if (std::search(encoded_elevations.begin(), encoded_elevations.end(),
                    expected_block.begin(), expected_block.end()) == encoded_elevations.end())
        return false;
    // And the default-valued encoding differs, so the block above was not found
    // because every handshake happens to contain it.
    if (encoded_elevations == encoded_handshake) return false;
    AgentMovementComplete complete;
    complete.agent_id = expected.agent_id;
    complete.session_id = expected.session_id;
    complete.region_handle = 0x0102030405060708ULL;
    const auto encoded_complete = encode_agent_movement_complete(complete);
    if (encoded_complete.size() <= 80 || encoded_complete[3] != std::byte{0xfa}) return false;
    const auto ping = encode_start_ping_check(7, 0x01020304);
    const auto ping_id = decode_start_ping_check(ping);
    const auto economy = encode_economy_data(0, 15000, 3);
    auto logout_payload = bytes({0xff, 0xff, 0x00, 0xfc});
    logout_payload.insert(logout_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    logout_payload.insert(logout_payload.end(), expected.session_id.begin(), expected.session_id.end());
    const auto logout = decode_logout_request(logout_payload);
    const auto logout_reply = logout ? encode_logout_reply(*logout) : std::vector<std::byte>{};
    auto create_folder_payload = bytes({0xff, 0xff, 0x01, 0x11});
    create_folder_payload.insert(create_folder_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    create_folder_payload.insert(create_folder_payload.end(), expected.session_id.begin(), expected.session_id.end());
    create_folder_payload.insert(create_folder_payload.end(), expected.session_id.begin(), expected.session_id.end());
    create_folder_payload.insert(create_folder_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    create_folder_payload.push_back(std::byte{0xff});
    create_folder_payload.push_back(std::byte{9});
    for (const char value : std::string("Projects\0", 9))
        create_folder_payload.push_back(static_cast<std::byte>(value));
    const auto create_folder = decode_create_inventory_folder(create_folder_payload);
    auto create_item_payload = bytes({0xff, 0xff, 0x01, 0x31});
    create_item_payload.insert(create_item_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    create_item_payload.insert(create_item_payload.end(), expected.session_id.begin(), expected.session_id.end());
    create_item_payload.insert(create_item_payload.end(),
                               {std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}});
    create_item_payload.insert(create_item_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    create_item_payload.insert(create_item_payload.end(), expected.session_id.begin(), expected.session_id.end());
    create_item_payload.insert(create_item_payload.end(),
                               {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x7f},
                                std::byte{5}, std::byte{18}, std::byte{5}, std::byte{10}});
    for (const char value : std::string("New Pants\0", 10))
        create_item_payload.push_back(static_cast<std::byte>(value));
    create_item_payload.insert(create_item_payload.end(), {std::byte{1}, std::byte{}});
    const auto create_item = decode_create_inventory_item(create_item_payload);
    if (!create_item || create_item->callback_id != 0x12345678 ||
        create_item->folder_id != expected.agent_id ||
        create_item->transaction_id != expected.session_id ||
        create_item->next_owner_permissions != 0x7fffffff ||
        create_item->asset_type != 5 || create_item->inventory_type != 18 ||
        create_item->wearable_type != 5 || create_item->name != "New Pants" ||
        !create_item->description.empty())
        return false;
    auto copy_item_payload = bytes({0xff, 0xff, 0x01, 0x0d});
    copy_item_payload.insert(copy_item_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    copy_item_payload.insert(copy_item_payload.end(), expected.session_id.begin(), expected.session_id.end());
    copy_item_payload.insert(copy_item_payload.end(),
                             {std::byte{1}, std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}});
    copy_item_payload.insert(copy_item_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    copy_item_payload.insert(copy_item_payload.end(), expected.session_id.begin(), expected.session_id.end());
    copy_item_payload.insert(copy_item_payload.end(), 16, std::byte{});
    copy_item_payload.push_back(std::byte{1});
    copy_item_payload.push_back(std::byte{});
    const auto copy_item = decode_copy_inventory_item(copy_item_payload);
    auto copy_item_without_name = copy_item_payload;
    copy_item_without_name.resize(copy_item_without_name.size() - 1);
    copy_item_without_name.back() = std::byte{};
    const auto unnamed_copy_item = decode_copy_inventory_item(copy_item_without_name);
    InventoryItem copied_item;
    copied_item.item_id = expected.session_id;
    copied_item.creator_id = expected.agent_id;
    copied_item.owner_id = expected.agent_id;
    copied_item.folder_id = expected.agent_id;
    copied_item.asset_id = expected.session_id;
    copied_item.asset_type = 5;
    copied_item.inventory_type = 18;
    copied_item.name = "Default Shirt";
    copied_item.flags = 4;
    copied_item.base_permissions = 0x7fffffff;
    copied_item.current_permissions = 0x7fffffff;
    copied_item.next_permissions = 0x7fffffff;
    if (!unnamed_copy_item || !unnamed_copy_item->new_name.empty()) return false;
    const auto copy_reply = copy_item ? encode_update_create_inventory_item(
        AgentMessage{expected.agent_id, expected.session_id}, copy_item->callback_id, copied_item)
                                      : std::vector<std::byte>{};
    auto move_folder_payload = bytes({0xff, 0xff, 0x01, 0x13});
    move_folder_payload.insert(move_folder_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    move_folder_payload.insert(move_folder_payload.end(), expected.session_id.begin(), expected.session_id.end());
    move_folder_payload.push_back(std::byte{1});
    move_folder_payload.push_back(std::byte{2});
    move_folder_payload.insert(move_folder_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    move_folder_payload.insert(move_folder_payload.end(), expected.session_id.begin(), expected.session_id.end());
    move_folder_payload.insert(move_folder_payload.end(), expected.session_id.begin(), expected.session_id.end());
    move_folder_payload.insert(move_folder_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    const auto move_folder = decode_move_inventory_folder(move_folder_payload);
    auto move_item_payload = bytes({0xff, 0xff, 0x01, 0x0c});
    move_item_payload.insert(move_item_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    move_item_payload.insert(move_item_payload.end(), expected.session_id.begin(), expected.session_id.end());
    move_item_payload.push_back(std::byte{1});
    move_item_payload.push_back(std::byte{2});
    move_item_payload.insert(move_item_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    move_item_payload.insert(move_item_payload.end(), expected.session_id.begin(), expected.session_id.end());
    move_item_payload.push_back(std::byte{});
    move_item_payload.insert(move_item_payload.end(), expected.session_id.begin(), expected.session_id.end());
    move_item_payload.insert(move_item_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    move_item_payload.push_back(std::byte{8});
    for (const char value : std::string("Renamed\0", 8))
        move_item_payload.push_back(static_cast<std::byte>(value));
    const auto move_item = decode_move_inventory_item(move_item_payload);
    std::vector<std::byte> object_add_payload(146);
    object_add_payload[0] = std::byte{0xff};
    object_add_payload[1] = std::byte{0x01};
    std::copy(expected.agent_id.begin(), expected.agent_id.end(), object_add_payload.begin() + 2);
    std::copy(expected.session_id.begin(), expected.session_id.end(), object_add_payload.begin() + 18);
    std::copy(expected.agent_id.begin(), expected.agent_id.end(), object_add_payload.begin() + 34);
    object_add_payload[50] = std::byte{9};
    object_add_payload[51] = std::byte{3};
    object_add_payload[52] = std::byte{0x02};
    object_add_payload[56] = std::byte{16};
    object_add_payload[57] = std::byte{1};
    object_add_payload[58] = std::byte{0x34};
    object_add_payload[59] = std::byte{0x12};
    object_add_payload[62] = std::byte{200};
    object_add_payload[63] = std::byte{100};
    object_add_payload[64] = std::byte{0xce};
    object_add_payload[72] = std::byte{7};
    object_add_payload[77] = std::byte{0x78};
    object_add_payload[78] = std::byte{0x56};
    object_add_payload[79] = std::byte{1};
    write_f32(object_add_payload, 80, 128.0F);
    write_f32(object_add_payload, 84, 128.0F);
    write_f32(object_add_payload, 88, 30.0F);
    write_f32(object_add_payload, 92, 132.0F);
    write_f32(object_add_payload, 96, 129.0F);
    write_f32(object_add_payload, 100, 22.0F);
    write_f32(object_add_payload, 121, 0.5F);
    write_f32(object_add_payload, 125, 0.75F);
    write_f32(object_add_payload, 129, 1.0F);
    const auto object_add = decode_object_add(object_add_payload);
    auto derez_payload = bytes({0xff, 0xff, 0x01, 0x23});
    derez_payload.insert(derez_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    derez_payload.insert(derez_payload.end(), expected.session_id.begin(), expected.session_id.end());
    derez_payload.insert(derez_payload.end(), 16, std::byte{});
    derez_payload.push_back(std::byte{6});
    derez_payload.insert(derez_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    derez_payload.insert(derez_payload.end(), expected.session_id.begin(), expected.session_id.end());
    derez_payload.insert(derez_payload.end(), {std::byte{1}, std::byte{0}, std::byte{2},
                                               std::byte{0x78}, std::byte{0x56},
                                               std::byte{0x34}, std::byte{0x12},
                                               std::byte{0x04}, std::byte{0x03},
                                               std::byte{0x02}, std::byte{0x01}});
    const auto derez = decode_derez_object(derez_payload);
    std::vector<std::byte> rez_payload(144);
    rez_payload[0] = std::byte{0xff};
    rez_payload[1] = std::byte{0xff};
    rez_payload[2] = std::byte{0x01};
    rez_payload[3] = std::byte{0x25};
    std::copy(expected.agent_id.begin(), expected.agent_id.end(), rez_payload.begin() + 4);
    std::copy(expected.session_id.begin(), expected.session_id.end(), rez_payload.begin() + 20);
    rez_payload[68] = std::byte{1};
    write_f32(rez_payload, 69, 120.0F);
    write_f32(rez_payload, 73, 121.0F);
    write_f32(rez_payload, 77, 30.0F);
    write_f32(rez_payload, 81, 130.0F);
    write_f32(rez_payload, 85, 131.0F);
    write_f32(rez_payload, 89, 22.0F);
    rez_payload[109] = std::byte{1};
    rez_payload[110] = std::byte{1};
    std::copy(expected.agent_id.begin(), expected.agent_id.end(), rez_payload.begin() + 128);
    const auto rez = decode_rez_object(rez_payload);
    const std::array<std::uint32_t, 2> killed_ids{0x12345678, 0x01020304};
    const auto killed = encode_kill_object(killed_ids);
    auto select_payload = bytes({0xff, 0xff, 0x00, 0x6e});
    select_payload.insert(select_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    select_payload.insert(select_payload.end(), expected.session_id.begin(), expected.session_id.end());
    select_payload.insert(select_payload.end(), {std::byte{1}, std::byte{0x78}, std::byte{0x56},
                                                  std::byte{0x34}, std::byte{0x12}});
    const auto selected = decode_object_select(select_payload);
    auto transform_payload = bytes({0xff, 0x02});
    transform_payload.insert(transform_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    transform_payload.insert(transform_payload.end(), expected.session_id.begin(), expected.session_id.end());
    transform_payload.insert(transform_payload.end(), {std::byte{1}, std::byte{0x78}, std::byte{0x56},
                                                        std::byte{0x34}, std::byte{0x12}, std::byte{0x0d},
                                                        std::byte{24}});
    const auto transform_data = transform_payload.size();
    transform_payload.resize(transform_data + 24);
    write_f32(transform_payload, transform_data, 130.0F);
    write_f32(transform_payload, transform_data + 4, 131.0F);
    write_f32(transform_payload, transform_data + 8, 25.0F);
    write_f32(transform_payload, transform_data + 12, 1.0F);
    write_f32(transform_payload, transform_data + 16, 2.0F);
    write_f32(transform_payload, transform_data + 20, 3.0F);
    const auto transform = decode_multiple_object_update(transform_payload);
    auto object_name_payload = bytes({0xff, 0xff, 0x00, 0x6b});
    object_name_payload.insert(object_name_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    object_name_payload.insert(object_name_payload.end(), expected.session_id.begin(), expected.session_id.end());
    object_name_payload.insert(object_name_payload.end(), {std::byte{1}, std::byte{0x78}, std::byte{0x56},
                                                            std::byte{0x34}, std::byte{0x12}, std::byte{6}});
    for (const char value : std::string("Prim1\0", 6))
        object_name_payload.push_back(static_cast<std::byte>(value));
    const auto object_name = decode_object_name(object_name_payload);
    auto object_description_payload = bytes({0xff, 0xff, 0x00, 0x6c});
    object_description_payload.insert(
        object_description_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    object_description_payload.insert(
        object_description_payload.end(), expected.session_id.begin(), expected.session_id.end());
    const std::string object_description_text = "Tall rotated box";
    object_description_payload.insert(object_description_payload.end(),
        {std::byte{1}, std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12},
         static_cast<std::byte>(object_description_text.size() + 1)});
    for (const char value : object_description_text)
        object_description_payload.push_back(static_cast<std::byte>(value));
    object_description_payload.push_back(std::byte{});
    const auto object_description = decode_object_description(object_description_payload);
    auto object_permissions_payload = bytes({0xff, 0xff, 0x00, 0x69});
    object_permissions_payload.insert(
        object_permissions_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    object_permissions_payload.insert(
        object_permissions_payload.end(), expected.session_id.begin(), expected.session_id.end());
    object_permissions_payload.insert(object_permissions_payload.end(),
        {std::byte{}, std::byte{1}, std::byte{0x78}, std::byte{0x56},
         std::byte{0x34}, std::byte{0x12}, std::byte{0x08}, std::byte{1},
         std::byte{}, std::byte{}, std::byte{0x08}, std::byte{}});
    const auto object_permissions = decode_object_permissions(object_permissions_payload);
    auto object_duplicate_payload = bytes({0xff, 0xff, 0x00, 0x5a});
    object_duplicate_payload.insert(
        object_duplicate_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    object_duplicate_payload.insert(
        object_duplicate_payload.end(), expected.session_id.begin(), expected.session_id.end());
    object_duplicate_payload.insert(
        object_duplicate_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    const auto duplicate_vectors = object_duplicate_payload.size();
    object_duplicate_payload.resize(duplicate_vectors + 12);
    write_f32(object_duplicate_payload, duplicate_vectors, 1.0F);
    write_f32(object_duplicate_payload, duplicate_vectors + 4, 2.0F);
    write_f32(object_duplicate_payload, duplicate_vectors + 8, 3.0F);
    object_duplicate_payload.insert(object_duplicate_payload.end(),
        {std::byte{0x02}, std::byte{}, std::byte{}, std::byte{}, std::byte{1},
         std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}});
    const auto object_duplicate = decode_object_duplicate(object_duplicate_payload);
    auto object_material_payload = bytes({0xff, 0xff, 0x00, 0x61});
    object_material_payload.insert(
        object_material_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    object_material_payload.insert(
        object_material_payload.end(), expected.session_id.begin(), expected.session_id.end());
    object_material_payload.insert(object_material_payload.end(),
        {std::byte{1}, std::byte{0x78}, std::byte{0x56}, std::byte{0x34},
         std::byte{0x12}, std::byte{0x01}});
    const auto object_material = decode_object_material(object_material_payload);
    auto object_shape_payload = bytes({0xff, 0xff, 0x00, 0x62});
    object_shape_payload.insert(
        object_shape_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    object_shape_payload.insert(
        object_shape_payload.end(), expected.session_id.begin(), expected.session_id.end());
    object_shape_payload.insert(object_shape_payload.end(),
        {std::byte{1}, std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12},
         std::byte{0x20}, std::byte{0x00}, std::byte{0x0a}, std::byte{0x00},
         std::byte{0x02}, std::byte{0x01}, std::byte{0x64}, std::byte{0x19},
         std::byte{0x02}, std::byte{0x03}, std::byte{0x04}, std::byte{0x05},
         std::byte{0x06}, std::byte{0x07}, std::byte{0x08}, std::byte{0x09},
         std::byte{0x0b}, std::byte{0x03}, std::byte{0x02}, std::byte{0x05},
         std::byte{0x04}, std::byte{0x88}, std::byte{0x13}});
    const auto object_shape = decode_object_shape(object_shape_payload);
    const auto truncated_object_shape = decode_object_shape(
        std::span(object_shape_payload).first(object_shape_payload.size() - 1));
    auto object_image_payload = bytes({0xff, 0xff, 0x00, 0x60});
    object_image_payload.insert(
        object_image_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    object_image_payload.insert(
        object_image_payload.end(), expected.session_id.begin(), expected.session_id.end());
    object_image_payload.insert(object_image_payload.end(),
        {std::byte{1}, std::byte{0x78}, std::byte{0x56}, std::byte{0x34},
         std::byte{0x12}, std::byte{}, std::byte{3}, std::byte{},
         std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}});
    const auto object_image = decode_object_image(object_image_payload);
    auto family_payload = bytes({0xff, 0x05});
    family_payload.insert(family_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    family_payload.insert(family_payload.end(), expected.session_id.begin(), expected.session_id.end());
    family_payload.insert(family_payload.end(), {std::byte{0x04}, std::byte{0x03},
                                                  std::byte{0x02}, std::byte{0x01}});
    family_payload.insert(family_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    const auto family = decode_request_object_properties_family(family_payload);
    ObjectProperties properties;
    properties.object_id = expected.agent_id;
    properties.creator_id = expected.agent_id;
    properties.owner_id = expected.session_id;
    properties.folded_owner_permissions &= ~0x00008000;
    properties.creation_date = 123;
    properties.name = "Primitive";
    const std::array property_list{properties};
    const auto encoded_properties = encode_object_properties(property_list);
    const auto encoded_family = encode_object_properties_family(0x01020304, properties);
    auto name_request_payload = bytes({0xff, 0xff, 0x00, 0xeb, 0x02});
    name_request_payload.insert(name_request_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    name_request_payload.insert(name_request_payload.end(), expected.session_id.begin(), expected.session_id.end());
    const auto name_request = decode_uuid_name_request(name_request_payload);
    const std::array names{
        UuidName{expected.agent_id, "Jim", "Tarber"},
        UuidName{expected.session_id, "Demo", "Avatar"}};
    const auto name_reply = encode_uuid_name_reply(names);
    auto cached_payload = bytes({0xff, 0xff, 0x01, 0x80});
    cached_payload.insert(cached_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    cached_payload.insert(cached_payload.end(), expected.session_id.begin(), expected.session_id.end());
    cached_payload.insert(cached_payload.end(), {std::byte{7}, std::byte{}, std::byte{}, std::byte{}, std::byte{2}});
    cached_payload.insert(cached_payload.end(), 16, std::byte{1});
    cached_payload.push_back(std::byte{8});
    cached_payload.insert(cached_payload.end(), 16, std::byte{2});
    cached_payload.push_back(std::byte{9});
    const auto cached = decode_agent_cached_texture(cached_payload);
    const auto cached_response = cached ? encode_agent_cached_texture_response(*cached) : std::vector<std::byte>{};
    auto appearance_payload = bytes({0xff, 0xff, 0x00, 0x54});
    appearance_payload.insert(appearance_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    appearance_payload.insert(appearance_payload.end(), expected.session_id.begin(), expected.session_id.end());
    appearance_payload.resize(52, std::byte{});
    write_f32(appearance_payload, 40, 0.45F);
    write_f32(appearance_payload, 44, 0.60F);
    write_f32(appearance_payload, 48, 2.0F);
    appearance_payload.push_back(std::byte{1});
    appearance_payload.insert(appearance_payload.end(), expected.session_id.begin(), expected.session_id.end());
    appearance_payload.push_back(std::byte{8});
    appearance_payload.insert(appearance_payload.end(), {std::byte{35}, std::byte{0}});
    appearance_payload.insert(appearance_payload.end(), expected.session_id.begin(), expected.session_id.end());
    appearance_payload.insert(appearance_payload.end(), {std::byte{0x82}, std::byte{0}});
    appearance_payload.insert(appearance_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    appearance_payload.push_back(std::byte{0});
    appearance_payload.push_back(std::byte{149});
    appearance_payload.insert(appearance_payload.end(), 149, std::byte{42});
    const auto appearance = decode_agent_set_appearance(appearance_payload);
    const auto avatar_appearance = appearance ? encode_avatar_appearance({
        appearance->agent_id, 17, appearance->texture_entry, appearance->visual_params,
        {0.0F, 0.0F, 0.125F}}) : std::vector<std::byte>{};
    auto image_payload = bytes({8});
    image_payload.insert(image_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    image_payload.insert(image_payload.end(), expected.session_id.begin(), expected.session_id.end());
    image_payload.push_back(std::byte{1});
    image_payload.insert(image_payload.end(), expected.agent_id.begin(), expected.agent_id.end());
    image_payload.insert(image_payload.end(),
                         {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x80}, std::byte{0x3f},
                          std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}});
    const auto image_request = decode_request_image(image_payload);
    const std::vector<std::byte> image_content(1601, std::byte{0x5a});
    const auto image_transfer = encode_image_transfer(expected.agent_id, image_content);
    const auto resumed_transfer = encode_image_transfer(expected.agent_id, image_content, 2);
    return ping == bytes({1, 7, 4, 3, 2, 1}) && ping_id && *ping_id == 7 &&
           !decode_start_ping_check(bytes({1, 7})) && encode_complete_ping_check(*ping_id) == bytes({2, 7}) &&
           is_economy_data_request(bytes({0xff, 0xff, 0x00, 0x18})) &&
           !is_economy_data_request(bytes({0xff, 0xff, 0x00, 0x19})) &&
           economy.size() == 72 && economy[3] == std::byte{0x19} &&
           economy[4] == std::byte{0x98} && economy[5] == std::byte{0x3a} &&
           economy[8] == std::byte{3} && economy[36] == std::byte{0} &&
           economy[52] == std::byte{0} && economy[53] == std::byte{0} &&
           economy[54] == std::byte{0x80} && economy[55] == std::byte{0x3f} &&
           logout && logout->agent_id == expected.agent_id && logout->session_id == expected.session_id &&
           create_folder && create_folder->agent_id == expected.agent_id &&
           create_folder->session_id == expected.session_id && create_folder->folder_id == expected.session_id &&
           create_folder->parent_id == expected.agent_id && create_folder->type == -1 &&
           create_folder->name == "Projects" &&
           copy_item && copy_item->agent_id == expected.agent_id &&
           copy_item->session_id == expected.session_id && copy_item->old_agent_id == expected.agent_id &&
           copy_item->old_item_id == expected.session_id && copy_item->callback_id == 0x12345678 &&
           copy_item->new_name.empty() && copy_reply.size() > 180 && copy_reply[3] == std::byte{0x0b} &&
           copy_reply[70] == std::byte{0x78} && copy_reply[71] == std::byte{0x56} &&
           move_folder && move_folder->agent_id == expected.agent_id &&
           move_folder->session_id == expected.session_id && move_folder->stamp &&
           move_folder->folders.size() == 2 && move_folder->folders[0].folder_id == expected.agent_id &&
           move_folder->folders[0].parent_id == expected.session_id &&
           move_folder->folders[1].folder_id == expected.session_id &&
           move_folder->folders[1].parent_id == expected.agent_id &&
           move_item && move_item->agent_id == expected.agent_id &&
           move_item->session_id == expected.session_id && move_item->stamp &&
           move_item->items.size() == 2 && move_item->items[0].item_id == expected.agent_id &&
           move_item->items[0].folder_id == expected.session_id && move_item->items[0].new_name.empty() &&
           move_item->items[1].item_id == expected.session_id &&
           move_item->items[1].folder_id == expected.agent_id && move_item->items[1].new_name == "Renamed" &&
           object_add && object_add->agent_id == expected.agent_id &&
           object_add->session_id == expected.session_id && object_add->group_id == expected.agent_id &&
           object_add->pcode == 9 && object_add->material == 3 && object_add->add_flags == 2 &&
           object_add->path_curve == 16 && object_add->profile_curve == 1 && object_add->bypass_raycast &&
           object_add->path_begin == 0x1234 && object_add->path_scale_x == 200 &&
           object_add->path_scale_y == 100 && object_add->path_shear_x == 0xce &&
           object_add->path_skew == 7 && object_add->profile_hollow == 0x5678 &&
           object_add->ray_start[2] == 30.0F && object_add->ray_end[0] == 132.0F &&
           object_add->scale[1] == 0.75F &&
           derez && derez->agent_id == expected.agent_id && derez->session_id == expected.session_id &&
           derez->destination == 6 && derez->destination_id == expected.agent_id &&
           derez->transaction_id == expected.session_id && derez->packet_count == 1 &&
           derez->packet_number == 0 && derez->local_ids == std::vector<std::uint32_t>(killed_ids.begin(), killed_ids.end()) &&
           valid_derez_batch(1, 0) && valid_derez_batch(1, 1) &&
           !valid_derez_batch(0, 0) && !valid_derez_batch(1, 2) &&
           rez && rez->agent_id == expected.agent_id && rez->session_id == expected.session_id &&
           rez->item_id == expected.agent_id && rez->bypass_raycast == 1 &&
           rez->ray_start[0] == 120.0F && rez->ray_end[1] == 131.0F &&
           rez->ray_end_is_intersection && rez->rez_selected && !rez->remove_item &&
           killed == bytes({0x10, 2, 0x78, 0x56, 0x34, 0x12, 0x04, 0x03, 0x02, 0x01}) &&
           selected && selected->agent_id == expected.agent_id && selected->session_id == expected.session_id &&
           selected->local_ids == std::vector<std::uint32_t>{0x12345678} &&
           transform && transform->agent_id == expected.agent_id &&
           transform->session_id == expected.session_id && transform->objects.size() == 1 &&
           transform->objects[0].local_id == 0x12345678 && transform->objects[0].type == 0x0d &&
           transform->objects[0].position && (*transform->objects[0].position)[1] == 131.0F &&
           !transform->objects[0].rotation && transform->objects[0].scale &&
           (*transform->objects[0].scale)[2] == 3.0F &&
           object_name && object_name->agent_id == expected.agent_id &&
           object_name->session_id == expected.session_id && object_name->objects.size() == 1 &&
           object_name->objects[0].local_id == 0x12345678 && object_name->objects[0].name == "Prim1" &&
           object_description && object_description->agent_id == expected.agent_id &&
           object_description->session_id == expected.session_id &&
           object_description->objects.size() == 1 &&
           object_description->objects[0].local_id == 0x12345678 &&
           object_description->objects[0].description == object_description_text &&
           object_permissions && object_permissions->agent_id == expected.agent_id &&
           object_permissions->session_id == expected.session_id &&
           !object_permissions->override_permissions && object_permissions->objects.size() == 1 &&
           object_permissions->objects[0].local_id == 0x12345678 &&
           object_permissions->objects[0].field == 0x08 && object_permissions->objects[0].set &&
           object_permissions->objects[0].mask == 0x00080000 &&
           object_duplicate && object_duplicate->agent_id == expected.agent_id &&
           object_duplicate->session_id == expected.session_id &&
           object_duplicate->group_id == expected.agent_id && object_duplicate->offset[0] == 1.0F &&
           object_duplicate->offset[1] == 2.0F && object_duplicate->offset[2] == 3.0F &&
           object_duplicate->duplicate_flags == 0x00000002 &&
           object_duplicate->local_ids == std::vector<std::uint32_t>{0x12345678} &&
           object_material && object_material->agent_id == expected.agent_id &&
           object_material->session_id == expected.session_id &&
           object_material->objects.size() == 1 &&
           object_material->objects[0].local_id == 0x12345678 &&
           object_material->objects[0].material == 0x01 &&
           object_shape && object_shape->agent_id == expected.agent_id &&
           object_shape->session_id == expected.session_id &&
           object_shape->objects.size() == 1 &&
           object_shape->objects[0].local_id == 0x12345678 &&
           object_shape->objects[0].path_curve == 0x20 &&
           object_shape->objects[0].profile_curve == 0x00 &&
           object_shape->objects[0].path_begin == 0x000a &&
           object_shape->objects[0].path_end == 0x0102 &&
           object_shape->objects[0].path_scale_x == 0x64 &&
           object_shape->objects[0].path_scale_y == 0x19 &&
           object_shape->objects[0].path_shear_x == 0x02 &&
           object_shape->objects[0].path_shear_y == 0x03 &&
           object_shape->objects[0].path_twist == 0x04 &&
           object_shape->objects[0].path_twist_begin == 0x05 &&
           object_shape->objects[0].path_radius_offset == 0x06 &&
           object_shape->objects[0].path_taper_x == 0x07 &&
           object_shape->objects[0].path_taper_y == 0x08 &&
           object_shape->objects[0].path_revolutions == 0x09 &&
           object_shape->objects[0].path_skew == 0x0b &&
           object_shape->objects[0].profile_begin == 0x0203 &&
           object_shape->objects[0].profile_end == 0x0405 &&
           object_shape->objects[0].profile_hollow == 0x1388 &&
           !truncated_object_shape &&
           object_image && object_image->agent_id == expected.agent_id &&
           object_image->session_id == expected.session_id && object_image->objects.size() == 1 &&
           object_image->objects[0].local_id == 0x12345678 &&
           object_image->objects[0].texture_entry == bytes({0xaa, 0xbb, 0xcc}) &&
           family && family->request_flags == 0x01020304 && family->object_id == expected.agent_id &&
           encoded_properties.size() > 180 && encoded_properties[0] == std::byte{0xff} &&
           encoded_properties[1] == std::byte{0x09} && encoded_properties[2] == std::byte{1} &&
           encoded_properties[75] == std::byte{0x00} && encoded_properties[76] == std::byte{0xe0} &&
           encoded_properties[77] == std::byte{0x09} && encoded_properties[104] == std::byte{0x3c} &&
           encoded_family.size() > 100 && encoded_family[1] == std::byte{0x0a} &&
           encoded_family[2] == std::byte{0x04} && encoded_family[54] == std::byte{0x00} &&
           encoded_family[55] == std::byte{0xe0} && encoded_family[56] == std::byte{0x09} &&
           name_request && name_request->size() == 2 && (*name_request)[0] == expected.agent_id &&
           (*name_request)[1] == expected.session_id && name_reply.size() == 64 &&
           name_reply[0] == std::byte{0xff} && name_reply[3] == std::byte{0xec} &&
           name_reply[4] == std::byte{2} && name_reply[21] == std::byte{4} &&
           logout_reply.size() == 53 && logout_reply[3] == std::byte{0xfd} && logout_reply[36] == std::byte{1} &&
           cached && cached->serial == 7 && cached->queries.size() == 2 &&
           cached->queries[0].texture_index == 8 && cached->queries[1].texture_index == 9 &&
           cached_response.size() == 79 && cached_response[3] == std::byte{0x81} &&
           cached_response[40] == std::byte{2} && cached_response[57] == std::byte{8} &&
           cached_response[76] == std::byte{9} && appearance && appearance->cache_entries.size() == 1 &&
           appearance->cache_entries[0].cache_id == expected.session_id &&
           appearance->cache_entries[0].texture_index == 8 && appearance->texture_ids[8] == expected.agent_id &&
           appearance->texture_ids[9] == expected.session_id && appearance->size[2] == 2.0F &&
           appearance->visual_params.size() == 149 && appearance->visual_params[148] == 42 &&
           avatar_appearance.size() == 232 &&
           avatar_appearance[0] == std::byte{0xff} && avatar_appearance[3] == std::byte{0x9e} &&
           avatar_appearance[21] == std::byte{35} && avatar_appearance[22] == std::byte{0} &&
           avatar_appearance[58] == std::byte{149} && avatar_appearance[208] == std::byte{1} &&
           image_request && image_request->requests.size() == 1 &&
           image_request->requests[0].image_id == expected.agent_id &&
           image_request->requests[0].download_priority == 1.0F && image_request->requests[0].type == 1 &&
           image_transfer.size() == 3 && image_transfer[0].size() == 626 &&
           image_transfer[0][0] == std::byte{9} && image_transfer[0][22] == std::byte{3} &&
           image_transfer[1].size() == 1021 && image_transfer[1][0] == std::byte{10} &&
           image_transfer[1][17] == std::byte{1} && image_transfer[2].size() == 22 &&
           resumed_transfer.size() == 1 && resumed_transfer[0] == image_transfer[2];
}

bool teleport_codecs() {
    const auto agent = *parse_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const auto session = *parse_uuid("11111111-2222-4333-8444-555555555555");
    std::vector<std::byte> request(68);
    request[0] = std::byte{0xff}; request[1] = std::byte{0xff};
    request[2] = std::byte{0x00}; request[3] = std::byte{0x3f};
    std::copy(agent.begin(), agent.end(), request.begin() + 4);
    std::copy(session.begin(), session.end(), request.begin() + 20);
    constexpr std::uint64_t handle = 0x0102030405060708ULL;
    for (std::size_t index = 0; index < 8; ++index)
        request[36 + index] = static_cast<std::byte>(handle >> (index * 8));
    write_f32(request, 44, 128.0F); write_f32(request, 48, 64.0F); write_f32(request, 52, 30.0F);
    write_f32(request, 56, 1.0F); write_f32(request, 60, 0.0F); write_f32(request, 64, 0.0F);
    const auto decoded = decode_teleport_location_request(request);
    if (!decoded || decoded->agent_id != agent || decoded->session_id != session ||
        decoded->region_handle != handle || decoded->position != std::array<float, 3>{128.0F, 64.0F, 30.0F} ||
        decoded->look_at != std::array<float, 3>{1.0F, 0.0F, 0.0F}) return false;
    write_f32(request, 44, std::numeric_limits<float>::quiet_NaN());
    if (decode_teleport_location_request(request)) return false;

    const auto start = encode_teleport_start(TeleportStart{0x00000010});
    if (start != bytes({0xff, 0xff, 0x00, 0x49, 0x10, 0x00, 0x00, 0x00})) return false;
    const auto local = encode_teleport_local(
        {agent, 2, {128.0F, 64.0F, 30.0F}, {1.0F, 0.0F, 0.0F}, 0x00002010});
    if (local.size() != 52 || local[3] != std::byte{0x40} ||
        !std::equal(agent.begin(), agent.end(), local.begin() + 4) ||
        local[20] != std::byte{2} || local[48] != std::byte{0x10} ||
        local[49] != std::byte{0x20}) return false;
    const auto failed = encode_teleport_failed(TeleportFailed{agent, "Destination unavailable"});
    return failed.size() == 4 + 16 + 1 + 24 + 1 && failed[3] == std::byte{0x4a} &&
           std::equal(agent.begin(), agent.end(), failed.begin() + 4) &&
           failed[20] == std::byte{24} && failed.back() == std::byte{};
}

bool map_codecs() {
    const auto agent = *parse_uuid("11111111-1111-4111-8111-111111111111");
    const auto session = *parse_uuid("22222222-2222-4222-8222-222222222222");
    auto block_payload = bytes({0xff, 0xff, 0x01, 0x97});
    block_payload.insert(block_payload.end(), agent.begin(), agent.end());
    block_payload.insert(block_payload.end(), session.begin(), session.end());
    block_payload.insert(block_payload.end(), {std::byte{4}, std::byte{}, std::byte{}, std::byte{}});
    block_payload.insert(block_payload.end(), 5, std::byte{});
    block_payload.insert(block_payload.end(), {
        std::byte{0xe8}, std::byte{0x03}, std::byte{0xe9}, std::byte{0x03},
        std::byte{0xe8}, std::byte{0x03}, std::byte{0xe8}, std::byte{0x03}});
    const auto block = decode_map_block_request(block_payload);
    if (!block || block->agent_id != agent || block->session_id != session || block->flags != 4 ||
        block->min_x != 1000 || block->max_x != 1001 || block->min_y != 1000 ||
        block->max_y != 1000) return false;

    auto name_payload = bytes({0xff, 0xff, 0x01, 0x98});
    name_payload.insert(name_payload.end(), agent.begin(), agent.end());
    name_payload.insert(name_payload.end(), session.begin(), session.end());
    name_payload.insert(name_payload.end(), 9, std::byte{});
    name_payload.push_back(std::byte{8});
    for (const char value : std::string("Sandbox\0", 8))
        name_payload.push_back(static_cast<std::byte>(value));
    const auto name = decode_map_name_request(name_payload);
    if (!name || name->name != "Sandbox") return false;

    const auto map_image = *parse_uuid("00000000-0000-1111-9999-000000000100");
    const std::array<MapBlock, 2> regions{{
        {1000, 1000, "Welcome", 13, 0, 20, 1, map_image, 512, 512},
        {1001, 1000, "Sandbox", 13, 0, 20, 0, map_image}}};
    const auto reply = encode_map_block_reply(agent, 4, regions);
    return reply.size() == 106 && reply[3] == std::byte{0x99} && reply[24] == std::byte{2} &&
           reply[25] == std::byte{0xe8} && reply[26] == std::byte{0x03} &&
           reply[29] == std::byte{8} && reply[30] == std::byte{'W'} &&
           std::equal(map_image.begin(), map_image.end(), reply.begin() + 45) &&
           reply[97] == std::byte{2} && reply[98] == std::byte{} && reply[99] == std::byte{2} &&
           reply[100] == std::byte{} && reply[101] == std::byte{2} &&
           reply[102] == std::byte{} && reply[103] == std::byte{1};
}

bool resend_throttle_and_timeout() {
    const auto start = Circuit::Clock::time_point{};
    Circuit circuit(start, 1200, 2s);
    if (!circuit.send(bytes({1}), true, start)) return false;
    if (!circuit.poll(start + 400ms).empty()) return false;
    const auto resend = circuit.poll(start + 600ms);
    if (resend.size() != 1) return false;
    const auto packet = decode_packet(resend.front());
    if (!packet || !(packet->flags & flag_resent)) return false;
    const std::vector<std::byte> oversized(2000, std::byte{1});
    if (circuit.send(oversized, false, start + 601ms)) return false;
    return !circuit.expired(start + 2s) && circuit.expired(start + 3s);
}

bool circuit_registry() {
    const auto start = Circuit::Clock::time_point{};
    const auto session = parse_uuid("11111111-2222-4333-8444-555555555555");
    const auto agent = parse_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    if (!session || !agent || parse_uuid("not-a-uuid")) return false;
    if (format_uuid(*session) != "11111111-2222-4333-8444-555555555555") return false;
    UseCircuitCode expected{987654, *session, *agent};
    unsigned authorizations = 0;
    CircuitRegistry registry([&](const UseCircuitCode& candidate) {
        ++authorizations;
        return candidate.circuit_code == expected.circuit_code && candidate.session_id == expected.session_id &&
               candidate.agent_id == expected.agent_id;
    });
    Packet opening;
    opening.flags = flag_reliable;
    opening.sequence = 42;
    opening.payload = encode_use_circuit_code(expected);
    const auto datagram = encode_packet(opening);
    if (!registry.receive("127.0.0.1:50000", datagram, start) || registry.size() != 1 || authorizations != 1)
        return false;
    if (!registry.identity("127.0.0.1:50000") ||
        registry.identity("127.0.0.1:50000")->circuit_code != expected.circuit_code)
        return false;
    if (!registry.receive("127.0.0.1:50001", datagram, start + 1ms) || registry.size() != 1 ||
        registry.identity("127.0.0.1:50000") || !registry.identity("127.0.0.1:50001"))
        return false;
    const auto replaced = registry.take_replaced();
    if (replaced.size() != 1 || replaced.front().endpoint != "127.0.0.1:50000" ||
        replaced.front().identity.agent_id != expected.agent_id)
        return false;
    const auto replies = registry.poll(start + 2ms);
    if (replies.size() != 1 || replies.front().endpoint != "127.0.0.1:50001") return false;
    const auto reply = decode_packet(replies.front().bytes);
    if (!reply || !decode_packet_ack(reply->payload)) return false;
    if (!registry.send("127.0.0.1:50001", bytes({7, 8}), true, start + 3ms) ||
        registry.send("127.0.0.1:50002", bytes({7, 8}), false, start + 3ms))
        return false;
    if (!registry.remove("127.0.0.1:50001") || registry.remove("127.0.0.1:50001") || registry.size() != 0)
        return false;
    if (!registry.receive("127.0.0.1:50000", datagram, start + 4ms)) return false;
    registry.poll(start + 31s);
    return registry.size() == 0;
}

bool agent_update_codec() {
    auto payload = bytes({4});
    const auto agent = parse_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const auto session = parse_uuid("11111111-2222-4333-8444-555555555555");
    if (!agent || !session) return false;
    payload.insert(payload.end(), agent->begin(), agent->end());
    payload.insert(payload.end(), session->begin(), session->end());
    payload.resize(115, std::byte{});
    // Body rotation x = 1.0, camera center x = 2.0, draw distance = 128.0.
    payload[35] = std::byte{0x80}; payload[36] = std::byte{0x3f};
    payload[60] = std::byte{0x00}; payload[61] = std::byte{0x40};
    payload[108] = std::byte{0x00}; payload[109] = std::byte{0x43};
    payload[110] = std::byte{0x01}; payload[111] = std::byte{0x20};
    const auto update = decode_agent_update(payload);
    return update && update->agent_id == *agent && update->session_id == *session &&
           update->body_rotation[0] == 1.0F && update->camera_center[0] == 2.0F &&
           update->draw_distance == 128.0F && update->control_flags == 0x2001;
}

bool modify_land_codec() {
    const auto agent = parse_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const auto session = parse_uuid("11111111-2222-4333-8444-555555555555");
    if (!agent || !session) return false;
    auto payload = bytes({0xff, 0xff, 0x00, 0x7c});
    payload.insert(payload.end(), agent->begin(), agent->end());
    payload.insert(payload.end(), session->begin(), session->end());
    payload.resize(72, std::byte{});
    payload[36] = std::byte{1};
    payload[37] = std::byte{2};
    write_f32(payload, 38, 0.5F);
    write_f32(payload, 42, 24.0F);
    payload[46] = std::byte{1};
    payload[47] = std::byte{0xff};
    payload[48] = std::byte{0xff};
    payload[49] = std::byte{0xff};
    payload[50] = std::byte{0xff};
    write_f32(payload, 51, 100.0F);
    write_f32(payload, 55, 101.0F);
    write_f32(payload, 59, 100.0F);
    write_f32(payload, 63, 101.0F);
    payload[67] = std::byte{1};
    write_f32(payload, 68, 4.0F);
    const auto decoded = decode_modify_land(payload);
    if (!decoded || decoded->agent_id != *agent || decoded->session_id != *session ||
        decoded->action != 1 || decoded->brush_size != 2 || decoded->seconds != 0.5F ||
        decoded->height != 24.0F || decoded->areas.size() != 1 ||
        decoded->areas[0].local_id != -1 || decoded->areas[0].west != 100.0F ||
        decoded->areas[0].north != 101.0F || decoded->extended_brush_sizes != std::vector<float>{4.0F})
        return false;
    payload.pop_back();
    return !decode_modify_land(payload);
}

bool animation_codecs() {
    const auto agent = parse_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const auto session = parse_uuid("11111111-2222-4333-8444-555555555555");
    const auto customize = parse_uuid("038fcec9-5ebd-8a8e-0e2e-6e71a0a1ac53");
    if (!agent || !session || !customize) return false;
    auto payload = bytes({5});
    payload.insert(payload.end(), agent->begin(), agent->end());
    payload.insert(payload.end(), session->begin(), session->end());
    payload.push_back(std::byte{1});
    payload.insert(payload.end(), customize->begin(), customize->end());
    payload.push_back(std::byte{1});
    payload.push_back(std::byte{});
    const auto incoming = decode_agent_animation(payload);
    if (!incoming || incoming->agent_id != *agent || incoming->session_id != *session ||
        incoming->animations.size() != 1 || incoming->animations[0].animation_id != *customize ||
        !incoming->animations[0].start)
        return false;
    const AvatarAnimation outgoing{*agent, {{*customize, 7, *agent}}};
    const auto encoded = encode_avatar_animation(outgoing);
    return encoded.size() == 56 && encoded[0] == std::byte{20} && encoded[17] == std::byte{1} &&
           encoded[34] == std::byte{7} && encoded[38] == std::byte{1} &&
           std::equal(agent->begin(), agent->end(), encoded.begin() + 39) &&
           encoded.back() == std::byte{};
}

bool wearable_asset_codecs() {
    const auto agent = parse_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const auto session = parse_uuid("11111111-2222-4333-8444-555555555555");
    const auto transaction = parse_uuid("99999999-8888-4777-8666-555555555555");
    const auto secure = parse_uuid("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    if (!agent || !session || !transaction || !secure ||
        format_uuid(combine_uuids(*transaction, *secure)) !=
            "a43650ca-1978-2e0b-9c01-1818739e7d32")
        return false;
    auto upload = bytes({0xff, 0xff, 0x01, 0x4d});
    upload.insert(upload.end(), transaction->begin(), transaction->end());
    upload.insert(upload.end(), {std::byte{13}, std::byte{}, std::byte{},
                                 std::byte{3}, std::byte{}, std::byte{1}, std::byte{2}, std::byte{3}});
    const auto decoded_upload = decode_asset_upload_request(upload);
    const auto complete = encode_asset_upload_complete(*agent, 13, true);
    if (!decoded_upload || decoded_upload->transaction_id != *transaction ||
        decoded_upload->asset_type != 13 || decoded_upload->data != bytes({1, 2, 3}) ||
        complete.size() != 22 || complete[3] != std::byte{0x4e} || complete.back() != std::byte{1})
        return false;

    auto update = bytes({0xff, 0xff, 0x01, 0x0a});
    update.insert(update.end(), agent->begin(), agent->end());
    update.insert(update.end(), session->begin(), session->end());
    update.insert(update.end(), 16, std::byte{}); // agent-data transaction
    update.push_back(std::byte{1});
    const auto block = update.size();
    update.resize(block + 132, std::byte{});
    std::copy(session->begin(), session->end(), update.begin() + block); // item ID
    std::copy(transaction->begin(), transaction->end(), update.begin() + block + 105);
    update.insert(update.end(), {std::byte{6}, std::byte{'S'}, std::byte{'h'}, std::byte{'a'},
                                 std::byte{'p'}, std::byte{'e'}, std::byte{},
                                 std::byte{1}, std::byte{}});
    update.insert(update.end(), 8, std::byte{});
    const auto decoded_update = decode_update_inventory_asset(update);
    const auto request_xfer = encode_request_xfer(0x0102030405060708ULL, *agent, 13);
    auto send_xfer = bytes({18, 8, 7, 6, 5, 4, 3, 2, 1, 3, 0, 0, 0, 3, 0, 9, 8, 7});
    const auto decoded_xfer = decode_send_xfer_packet(send_xfer);
    const auto confirmed = encode_confirm_xfer_packet(0x0102030405060708ULL, 3);
    return decoded_update && decoded_update->agent_id == *agent &&
           decoded_update->session_id == *session && decoded_update->item_id == *session &&
           decoded_update->transaction_id == *transaction && request_xfer.size() == 35 &&
           request_xfer[3] == std::byte{0x9c} && decoded_xfer &&
           decoded_xfer->id == 0x0102030405060708ULL && decoded_xfer->packet == 3 &&
           decoded_xfer->data == bytes({9, 8, 7}) && confirmed.size() == 13 &&
           confirmed[0] == std::byte{19};
}

bool chat_codecs() {
    const auto agent = parse_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const auto session = parse_uuid("11111111-2222-4333-8444-555555555555");
    if (!agent || !session) return false;
    auto payload = bytes({0xff, 0xff, 0x00, 0x50});
    payload.insert(payload.end(), agent->begin(), agent->end());
    payload.insert(payload.end(), session->begin(), session->end());
    payload.insert(payload.end(), {std::byte{3}, std::byte{}, std::byte{'h'}, std::byte{'i'}, std::byte{},
                                   std::byte{1}, std::byte{}, std::byte{}, std::byte{}, std::byte{}});
    const auto incoming = decode_chat_from_viewer(payload);
    if (!incoming || incoming->message != "hi" || incoming->type != 1 || incoming->channel != 0) return false;
    ChatFromSimulator outgoing{"Test User", *agent, *agent, 1, 1, 1, {1.F, 2.F, 3.F}, "hello"};
    const auto encoded = encode_chat_from_simulator(outgoing);
    return encoded.size() > 60 && encoded[3] == std::byte{0x8b};
}

bool flat_terrain_codec() {
    const std::array<TerrainPatch, 2> patches{{{1, 0}, {15, 15}}};
    const auto terrain = encode_flat_terrain(patches, 25.0F);
    if (terrain.size() < 20 || terrain[0] != std::byte{11} || terrain[1] != std::byte{0x4c}) return false;
    const auto length = std::to_integer<unsigned>(terrain[2]) |
                        (std::to_integer<unsigned>(terrain[3]) << 8);
    if (!(length + 4 == terrain.size() && terrain[4] == std::byte{8} && terrain[5] == std::byte{1} &&
           terrain[6] == std::byte{16} && terrain[7] == std::byte{0x4c} && terrain[8] == std::byte{0x84} &&
           terrain[9] == std::byte{} && terrain[10] == std::byte{} && terrain[11] == std::byte{0xc4} &&
           terrain[12] == std::byte{0x41} && terrain[13] == std::byte{1} && terrain[14] == std::byte{} &&
           terrain[15] == std::byte{0x20} && terrain[16] == std::byte{0x28})) return false;
    std::array<float, 256 * 256> heightmap{};
    for (std::size_t y = 0; y < 256; ++y)
        for (std::size_t x = 0; x < 256; ++x)
            heightmap[y * 256 + x] = 10.0F + static_cast<float>(x + y) / 16.0F;
    const auto shaped = encode_terrain(patches, heightmap);
    return shaped.size() > terrain.size() && shaped[0] == std::byte{11} && shaped[1] == std::byte{0x4c} &&
           encode_terrain(patches, std::span<const float>(heightmap.data(), 100)).empty();
}

bool extended_terrain_codec() {
    std::vector<float> medium(512 * 512, 20.0F);
    medium[400 * 512 + 300] = 24.0F;
    const std::array<TerrainPatch, 1> medium_patch{{{18, 25}}};
    const auto encoded_medium = encode_terrain(medium_patch, medium);
    if (encoded_medium.size() <= 20 || encoded_medium[0] != std::byte{11} ||
        encoded_medium[1] != std::byte{0x4d}) return false;

    std::vector<float> maximum(1024 * 1024, 22.0F);
    maximum[900 * 1024 + 800] = 28.0F;
    const std::array<TerrainPatch, 1> maximum_patch{{{50, 56}}};
    const auto encoded_maximum = encode_terrain(maximum_patch, maximum);
    const std::array<TerrainPatch, 1> invalid_patch{{{64, 0}}};
    return encoded_maximum.size() > 20 && encoded_maximum[1] == std::byte{0x4d} &&
           encode_terrain(invalid_patch, maximum).empty();
}

bool static_object_codec() {
    StaticObject object;
    object.parent_local_id = 0x12345678;
    object.id = *parse_uuid("12345678-1234-4234-8234-123456789abc");
    object.update_flags = 0x1002013c;
    object.rotation = {0.25F, 0.0F, 0.0F};
    object.path_curve = 0x20;
    object.profile_curve = 0x05;
    object.path_begin = 0x1234;
    object.path_scale_x = 200;
    object.path_scale_y = 100;
    object.path_shear_x = 0xce;
    object.path_skew = 7;
    object.profile_hollow = 0x5678;
    const auto encoded = encode_static_object_update(0x0102030405060708ULL, object);
    if (encoded.size() <= 220 || encoded[0] != std::byte{12} || encoded[1] != std::byte{8} ||
        encoded[8] != std::byte{1} || encoded[11] != std::byte{1} || encoded[37] != std::byte{9} ||
        encoded[89] != std::byte{0x00} || encoded[90] != std::byte{0x00} ||
        encoded[91] != std::byte{0x80} || encoded[92] != std::byte{0x3e} ||
        encoded[113] != std::byte{0x78} || encoded[114] != std::byte{0x56} ||
        encoded[115] != std::byte{0x34} || encoded[116] != std::byte{0x12} ||
        encoded[117] != std::byte{0x3c} || encoded[118] != std::byte{0x01} ||
        encoded[119] != std::byte{0x02} || encoded[120] != std::byte{0x10} ||
        encoded[121] != std::byte{0x20} || encoded[122] != std::byte{0x05} ||
        encoded[123] != std::byte{0x34} || encoded[124] != std::byte{0x12} ||
        encoded[127] != std::byte{200} || encoded[128] != std::byte{100} ||
        encoded[129] != std::byte{0xce} || encoded[137] != std::byte{7} ||
        encoded[142] != std::byte{0x78} || encoded[143] != std::byte{0x56})
        return false;
    const auto agent = *parse_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const auto avatar = encode_avatar_object_update(0x0102030405060708ULL, 42, agent, {128.F, 128.F, 25.F});
    const auto moving_avatar = encode_avatar_object_update(
        0x0102030405060708ULL, 42, agent, {128.F, 128.F, 25.F}, {1.F, 2.F, 3.F}, {0.F, 0.F, 0.5F});
    return avatar.size() == encoded.size() && moving_avatar.size() == avatar.size() &&
           moving_avatar != avatar && avatar[37] == std::byte{47} && avatar[38] == std::byte{4} &&
           std::equal(agent.begin(), agent.end(), avatar.begin() + 17);
}

bool object_relationship_codecs() {
    const auto agent = *parse_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const auto session = *parse_uuid("11111111-2222-4333-8444-555555555555");
    const auto make_request = [&](std::uint8_t message) {
        auto payload = bytes({0xff, 0xff, 0x00, message});
        payload.insert(payload.end(), agent.begin(), agent.end());
        payload.insert(payload.end(), session.begin(), session.end());
        payload.push_back(std::byte{3});
        const auto local_ids = bytes({42, 0, 0, 0, 7, 0, 0, 0, 99, 0, 0, 0});
        payload.insert(payload.end(), local_ids.begin(), local_ids.end());
        return payload;
    };
    const auto linked = decode_object_link(make_request(0x73));
    const auto delinked = decode_object_delink(make_request(0x74));
    auto malformed = make_request(0x73);
    malformed.pop_back();
    return linked && linked->agent_id == agent && linked->session_id == session &&
           linked->local_ids == std::vector<std::uint32_t>{42, 7, 99} &&
           delinked && delinked->local_ids == linked->local_ids &&
           !decode_object_link(malformed) && !decode_object_link(make_request(0x74));
}

bool object_flag_codec() {
    const auto agent = *parse_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const auto session = *parse_uuid("11111111-2222-4333-8444-555555555555");
    std::vector<std::byte> payload{std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x5e}};
    payload.insert(payload.end(), agent.begin(), agent.end());
    payload.insert(payload.end(), session.begin(), session.end());
    payload.insert(payload.end(), {std::byte{0x2a}, std::byte{}, std::byte{}, std::byte{},
                                   std::byte{1}, std::byte{}, std::byte{1}, std::byte{1}, std::byte{1},
                                   std::byte{2}});
    const auto extra_offset = payload.size();
    payload.resize(payload.size() + 16);
    write_f32(payload, extra_offset, 125.0F);
    write_f32(payload, extra_offset + 4, 0.7F);
    write_f32(payload, extra_offset + 8, 0.25F);
    write_f32(payload, extra_offset + 12, 1.5F);
    const auto decoded = decode_object_flag_update(payload);
    return decoded && decoded->agent_id == agent && decoded->session_id == session &&
           decoded->local_id == 42 && decoded->use_physics && !decoded->temporary &&
           decoded->phantom && decoded->casts_shadows && decoded->has_extra_physics &&
           decoded->physics_shape_type == 2 && decoded->density == 125.0F &&
           decoded->friction == 0.7F && decoded->restitution == 0.25F &&
           decoded->gravity_multiplier == 1.5F;
}

bool object_interaction_codecs() {
    const auto agent = *parse_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const auto session = *parse_uuid("11111111-2222-4333-8444-555555555555");
    const auto object = *parse_uuid("12345678-1234-4234-8234-123456789abc");
    auto deselect = bytes({0xff, 0xff, 0x00, 0x6f});
    deselect.insert(deselect.end(), agent.begin(), agent.end());
    deselect.insert(deselect.end(), session.begin(), session.end());
    deselect.insert(deselect.end(), {std::byte{1}, std::byte{0x2a}, std::byte{},
                                     std::byte{}, std::byte{}});
    const auto decoded_deselect = decode_object_deselect(deselect);
    if (!decoded_deselect || decoded_deselect->agent_id != agent ||
        decoded_deselect->session_id != session ||
        decoded_deselect->local_ids != std::vector<std::uint32_t>{42})
        return false;

    std::vector<std::byte> grab(81);
    grab[0] = std::byte{0xff};
    grab[1] = std::byte{0xff};
    grab[2] = std::byte{0x00};
    grab[3] = std::byte{0x76};
    std::copy(agent.begin(), agent.end(), grab.begin() + 4);
    std::copy(session.begin(), session.end(), grab.begin() + 20);
    std::copy(object.begin(), object.end(), grab.begin() + 36);
    write_f32(grab, 52, 0.25F);
    write_f32(grab, 56, -0.5F);
    write_f32(grab, 60, 0.75F);
    write_f32(grab, 64, 100.0F);
    write_f32(grab, 68, 101.0F);
    write_f32(grab, 72, 24.0F);
    grab[76] = std::byte{25};
    const auto decoded_grab = decode_object_grab_update(grab);
    if (!decoded_grab || decoded_grab->agent_id != agent ||
        decoded_grab->session_id != session || decoded_grab->object_id != object ||
        decoded_grab->grab_offset_initial != std::array<float, 3>{0.25F, -0.5F, 0.75F} ||
        decoded_grab->grab_position != std::array<float, 3>{100.0F, 101.0F, 24.0F} ||
        decoded_grab->time_since_last != 25)
        return false;

    // ObjectGrab (Low 117) is the initial touch: LocalID + GrabOffset, then a
    // variable SurfaceInfo array whose declared count must match the payload.
    std::vector<std::byte> touch(53);
    touch[0] = std::byte{0xff};
    touch[1] = std::byte{0xff};
    touch[2] = std::byte{0x00};
    touch[3] = std::byte{0x75};
    std::copy(agent.begin(), agent.end(), touch.begin() + 4);
    std::copy(session.begin(), session.end(), touch.begin() + 20);
    touch[36] = std::byte{0x2a}; // LocalID 42, little-endian
    write_f32(touch, 40, 0.5F);
    write_f32(touch, 44, -0.25F);
    write_f32(touch, 48, 1.5F);
    touch[52] = std::byte{0}; // zero SurfaceInfo blocks
    const auto decoded_touch = decode_object_grab(touch);
    if (!decoded_touch || decoded_touch->agent_id != agent ||
        decoded_touch->session_id != session || decoded_touch->local_id != 42 ||
        decoded_touch->grab_offset != std::array<float, 3>{0.5F, -0.25F, 1.5F})
        return false;
    // A SurfaceInfo count that does not match the trailing bytes is rejected.
    touch[52] = std::byte{1};
    if (decode_object_grab(touch)) return false;
    // ObjectGrabUpdate (118) must not decode as ObjectGrab.
    return !decode_object_grab(grab);
}

bool default_primitive_texture() {
    const auto plywood = *parse_uuid("89556747-24cb-43ed-920b-47caed15465f");
    const auto entry = default_texture_entry(plywood);
    if (entry.size() != 63 || !std::equal(plywood.begin(), plywood.end(), entry.begin())) return false;
    const auto expected_tail = bytes({
        0x00,                         // texture overrides
        0x00, 0x00, 0x00, 0x00,     // inverted white
        0x00,                         // color overrides
        0x00, 0x00, 0x80, 0x3f, 0x00, // U repeat and overrides
        0x00, 0x00, 0x80, 0x3f, 0x00  // V repeat and overrides
    });
    if (!std::equal(expected_tail.begin(), expected_tail.end(), entry.begin() + 16) ||
        !std::all_of(entry.begin() + 32, entry.end(), [](std::byte value) { return value == std::byte{}; }))
        return false;
    auto absent = std::vector<std::byte>{};
    if (!normalize_primitive_texture_entry(absent, entry) || absent != entry) return false;
    auto null_default = entry;
    std::fill_n(null_default.begin(), 16, std::byte{});
    null_default[18] = std::byte{0x7f};
    if (!normalize_primitive_texture_entry(null_default, entry) ||
        !std::equal(plywood.begin(), plywood.end(), null_default.begin()) ||
        null_default[18] != std::byte{0x7f}) return false;
    auto viewer_default = entry;
    const auto fallback = *parse_uuid("d2114404-dd59-4a4d-8e6c-49359e91bbf0");
    std::copy(fallback.begin(), fallback.end(), viewer_default.begin());
    viewer_default[19] = std::byte{0x55};
    if (!normalize_primitive_texture_entry(viewer_default, entry) ||
        !std::equal(plywood.begin(), plywood.end(), viewer_default.begin()) ||
        viewer_default[19] != std::byte{0x55}) return false;
    return !normalize_primitive_texture_entry(viewer_default, entry);
}

bool baked_texture_entry_roundtrip() {
    const auto default_id = *parse_uuid("5748decc-f629-461c-9a36-a35a221fe21f");
    std::array<Uuid, 32> faces;
    faces.fill(default_id);
    // Distinct baked UUIDs at the six classic bake slots (head/upper/lower/
    // eyes/skirt/hair). Index 19/20 force multi-byte face bitmaps.
    faces[8] = *parse_uuid("11111111-0000-4000-8000-000000000008");
    faces[9] = *parse_uuid("22222222-0000-4000-8000-000000000009");
    faces[10] = *parse_uuid("33333333-0000-4000-8000-000000000010");
    faces[11] = *parse_uuid("44444444-0000-4000-8000-000000000011");
    faces[19] = *parse_uuid("55555555-0000-4000-8000-000000000019");
    faces[20] = *parse_uuid("66666666-0000-4000-8000-000000000020");
    // Two faces on opposite sides of a 7-bit bitmap boundary sharing one UUID
    // exercises override grouping and a multi-byte bitmap.
    const auto shared = *parse_uuid("77777777-0000-4000-8000-000000000077");
    faces[0] = shared;
    faces[7] = shared;

    const auto encoded = encode_avatar_texture_entry(faces, default_id);
    const auto decoded = unpack_texture_entry_faces(encoded);
    if (!decoded || *decoded != faces) return false;

    // The non-texture attribute tail must match default_texture_entry's tail
    // (46 bytes: color..render material) so viewers parse the whole blob.
    const auto reference = default_texture_entry(default_id);
    if (encoded.size() < 46 || reference.size() < 46) return false;
    if (!std::equal(reference.end() - 46, reference.end(), encoded.end() - 46)) return false;

    // Faces left at the default must not emit overrides: with no non-default
    // faces the blob equals default_texture_entry exactly.
    std::array<Uuid, 32> all_default;
    all_default.fill(default_id);
    if (encode_avatar_texture_entry(all_default, default_id) != reference) return false;
    return true;
}

bool transfer_codecs() {
    const auto transfer = parse_uuid("12345678-90ab-4cde-8f01-234567890abc");
    const auto agent = parse_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const auto session = parse_uuid("11111111-2222-4333-8444-555555555555");
    const auto task = parse_uuid("00000000-0000-0000-0000-000000000000");
    const auto item = parse_uuid("22222222-3333-4444-8555-666666666666");
    const auto asset = parse_uuid("33333333-4444-4555-8666-777777777777");
    if (!transfer || !agent || !session || !task || !item || !asset) return false;

    const auto u32 = [](std::vector<std::byte>& out, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xff));
    };

    std::vector<std::byte> params; // SIM_INV_ITEM: agent, session, owner, task, item, asset, type
    for (const auto* id : {&*agent, &*session, &*agent, &*task, &*item, &*asset})
        params.insert(params.end(), id->begin(), id->end());
    u32(params, 10); // asset type = LSL text
    if (params.size() != 100) return false;

    auto payload = bytes({0xff, 0xff, 0x00, 0x99});
    payload.insert(payload.end(), transfer->begin(), transfer->end());
    u32(payload, static_cast<std::uint32_t>(transfer_channel_asset));
    u32(payload, static_cast<std::uint32_t>(transfer_source_sim_inv_item));
    u32(payload, 0); // priority (F32 bits 0.0)
    payload.push_back(static_cast<std::byte>(params.size() & 0xff));
    payload.push_back(static_cast<std::byte>((params.size() >> 8) & 0xff));
    payload.insert(payload.end(), params.begin(), params.end());

    const auto decoded = decode_transfer_request(payload);
    if (!decoded) return false;
    if (decoded->transfer_id != *transfer || decoded->channel_type != transfer_channel_asset ||
        decoded->source_type != transfer_source_sim_inv_item)
        return false;
    if (decoded->agent_id != *agent || decoded->session_id != *session ||
        decoded->item_id != *item || decoded->asset_id != *asset || decoded->asset_type != 10)
        return false;
    if (decoded->params.size() != 100) return false;

    // TransferInfo echoes the params and is Low 154.
    const auto info = encode_transfer_info(*transfer, transfer_channel_asset, transfer_status_ok,
                                           42, decoded->params);
    if (info.size() != 4 + 16 + 4 + 4 + 4 + 4 + 2 + 100) return false;
    if (info[0] != std::byte{0xff} || info[1] != std::byte{0xff} || info[2] != std::byte{0x00} ||
        info[3] != std::byte{0x9a})
        return false;

    // TransferPacket is High 17 and carries the (final) chunk.
    const auto data = bytes({'d', 'e', 'f', 'a', 'u', 'l', 't'});
    const auto pkt = encode_transfer_packet(*transfer, transfer_channel_asset, 0,
                                            transfer_status_done, data);
    if (pkt.empty() || pkt[0] != std::byte{17}) return false;
    if (pkt.size() != 1 + 16 + 4 + 4 + 4 + 2 + data.size()) return false;

    // A different message id must be rejected.
    auto wrong = payload;
    wrong[3] = std::byte{0x9c};
    return !decode_transfer_request(wrong);
}
}

bool home_and_gesture_codecs() {
    const auto agent = *parse_uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const auto session = *parse_uuid("11111111-2222-4333-8444-555555555555");

    // SetStartLocationRequest (Low 324): SimName "Sandbox", LocationID, pos, lookAt.
    auto set_home = bytes({0xff, 0xff, 0x01, 0x44});
    set_home.insert(set_home.end(), agent.begin(), agent.end());
    set_home.insert(set_home.end(), session.begin(), session.end());
    set_home.push_back(std::byte{7});
    for (const char value : std::string("Sandbox")) set_home.push_back(static_cast<std::byte>(value));
    set_home.insert(set_home.end(), 4, std::byte{}); // LocationID = 0
    const std::size_t pos_offset = set_home.size();
    set_home.insert(set_home.end(), 24, std::byte{}); // LocationPos(12) + LocationLookAt(12)
    write_f32(set_home, pos_offset, 128.0F);
    write_f32(set_home, pos_offset + 4, 64.0F);
    write_f32(set_home, pos_offset + 8, 25.0F);
    write_f32(set_home, pos_offset + 12, 1.0F);
    const auto home = decode_set_start_location_request(set_home);
    if (!home || home->agent_id != agent || home->session_id != session ||
        home->position != std::array<float, 3>{128.0F, 64.0F, 25.0F}) return false;

    const auto item1 = *parse_uuid("00000000-0000-4000-8000-000000000001");
    const auto asset1 = *parse_uuid("00000000-0000-4000-8000-0000000000a1");
    const auto item2 = *parse_uuid("00000000-0000-4000-8000-000000000002");
    const auto asset2 = *parse_uuid("00000000-0000-4000-8000-0000000000a2");

    // ActivateGestures (Low 316): Flags(U32), count byte, two {ItemID, AssetID, GestureFlags}.
    auto activate = bytes({0xff, 0xff, 0x01, 0x3c});
    activate.insert(activate.end(), agent.begin(), agent.end());
    activate.insert(activate.end(), session.begin(), session.end());
    activate.insert(activate.end(), 4, std::byte{}); // Flags
    activate.push_back(std::byte{2});                 // Data count
    activate.insert(activate.end(), item1.begin(), item1.end());
    activate.insert(activate.end(), asset1.begin(), asset1.end());
    activate.insert(activate.end(), 4, std::byte{});
    activate.insert(activate.end(), item2.begin(), item2.end());
    activate.insert(activate.end(), asset2.begin(), asset2.end());
    activate.insert(activate.end(), 4, std::byte{});
    const auto activated = decode_activate_gestures(activate);
    if (!activated || activated->agent_id != agent || activated->session_id != session ||
        activated->gestures.size() != 2 ||
        activated->gestures[0].item_id != item1 || activated->gestures[0].asset_id != asset1 ||
        activated->gestures[1].item_id != item2 || activated->gestures[1].asset_id != asset2) return false;

    // DeactivateGestures (Low 317): Flags(U32), count byte, one {ItemID, GestureFlags}.
    auto deactivate = bytes({0xff, 0xff, 0x01, 0x3d});
    deactivate.insert(deactivate.end(), agent.begin(), agent.end());
    deactivate.insert(deactivate.end(), session.begin(), session.end());
    deactivate.insert(deactivate.end(), 4, std::byte{}); // Flags
    deactivate.push_back(std::byte{1});                   // Data count
    deactivate.insert(deactivate.end(), item1.begin(), item1.end());
    deactivate.insert(deactivate.end(), 4, std::byte{});
    const auto deactivated = decode_deactivate_gestures(deactivate);
    return deactivated && deactivated->agent_id == agent && deactivated->session_id == session &&
           deactivated->item_ids.size() == 1 && deactivated->item_ids[0] == item1;
}

bool parcel_codecs() {
    // ParcelPropertiesRequest (Medium 11): [ff 0b][agent 16][session 16][seq][w][s][e][n][snap]
    {
        std::vector<std::byte> payload(55, std::byte{0});
        payload[0] = std::byte{0xff};
        payload[1] = std::byte{0x0b};
        payload[2] = std::byte{0x11}; // agent id first byte
        payload[18] = std::byte{0x22}; // session id first byte
        write_u32(payload, 34, static_cast<std::uint32_t>(-1));
        write_f32(payload, 38, 32.0F);
        write_f32(payload, 42, 48.0F);
        write_f32(payload, 46, 64.0F);
        write_f32(payload, 50, 80.0F);
        payload[54] = std::byte{1};
        const auto request = decode_parcel_properties_request(payload);
        if (!request || request->sequence_id != -1 || request->west != 32.0F ||
            request->north != 80.0F || !request->snap_selection ||
            std::to_integer<int>(request->agent_id[0]) != 0x11 ||
            std::to_integer<int>(request->session_id[0]) != 0x22)
            return false;
    }
    // ParcelPropertiesRequestByID (Low 197): [ff ff 00 c5][agent][session][seq][local]
    {
        std::vector<std::byte> payload(44, std::byte{0});
        payload[0] = std::byte{0xff}; payload[1] = std::byte{0xff};
        payload[2] = std::byte{0x00}; payload[3] = std::byte{0xc5};
        write_u32(payload, 36, static_cast<std::uint32_t>(-2));
        write_u32(payload, 40, 7);
        const auto request = decode_parcel_properties_request_by_id(payload);
        if (!request || request->sequence_id != -2 || request->local_id != 7) return false;
    }
    // ParcelDivide (Low 211) and ParcelJoin (Low 210).
    {
        std::vector<std::byte> payload(52, std::byte{0});
        payload[0] = std::byte{0xff}; payload[1] = std::byte{0xff};
        payload[2] = std::byte{0x00}; payload[3] = std::byte{0xd3};
        write_f32(payload, 36, 4.0F);
        write_f32(payload, 40, 8.0F);
        write_f32(payload, 44, 12.0F);
        write_f32(payload, 48, 16.0F);
        const auto divide = decode_parcel_divide(payload);
        if (!divide || divide->west != 4.0F || divide->north != 16.0F) return false;
        if (decode_parcel_join(payload)) return false; // wrong message number
        payload[3] = std::byte{0xd2};
        const auto join = decode_parcel_join(payload);
        if (!join || join->east != 12.0F) return false;
    }
    // ParcelAccessListRequest (Low 215): [id][agent][session][seq][flags][local]
    {
        std::vector<std::byte> payload(48, std::byte{0});
        payload[0] = std::byte{0xff}; payload[1] = std::byte{0xff};
        payload[2] = std::byte{0x00}; payload[3] = std::byte{0xd7};
        write_u32(payload, 36, 3);   // sequence
        write_u32(payload, 40, 2);   // flags = Ban
        write_u32(payload, 44, 5);   // local id
        const auto request = decode_parcel_access_list_request(payload);
        if (!request || request->sequence_id != 3 || request->flags != 2 || request->local_id != 5)
            return false;
    }
    // ParcelAccessListUpdate (Low 217) with one entry.
    {
        std::vector<std::byte> payload(68 + 1 + 24, std::byte{0});
        payload[0] = std::byte{0xff}; payload[1] = std::byte{0xff};
        payload[2] = std::byte{0x00}; payload[3] = std::byte{0xd9};
        write_u32(payload, 36, 1);   // flags = Access
        write_u32(payload, 40, 9);   // local id
        write_u32(payload, 60, 0);   // sequence
        write_u32(payload, 64, 1);   // sections
        payload[68] = std::byte{1};  // one entry
        payload[69] = std::byte{0xab}; // entry id first byte
        write_u32(payload, 69 + 16, 42);   // time
        write_u32(payload, 69 + 20, 1);    // flags
        const auto update = decode_parcel_access_list_update(payload);
        if (!update || update->flags != 1 || update->local_id != 9 || update->entries.size() != 1 ||
            update->entries[0].time != 42 ||
            std::to_integer<int>(update->entries[0].id[0]) != 0xab)
            return false;
    }
    // ParcelAccessListReply encoder: empty list emits one zero-UUID entry.
    {
        ParcelAccessListReply reply;
        reply.sequence_id = 0;
        reply.flags = 1;
        reply.local_id = 3;
        const auto encoded = encode_parcel_access_list_reply(reply);
        // prefix(4) + agent(16) + seq(4) + flags(4) + local(4) + count(1) + one entry(24)
        if (encoded.size() != 4 + 16 + 12 + 1 + 24) return false;
        if (encoded[0] != std::byte{0xff} || encoded[3] != std::byte{0xd8}) return false;
        // prefix(4)+agent(16)+seq(4)+flags(4)+local(4) = 32, then the count byte.
        if (encoded[32] != std::byte{1}) return false; // count == 1 (the zero entry)
    }
    // ParcelObjectOwnersRequest (Low 56) and Reply (Low 57).
    {
        std::vector<std::byte> payload(40, std::byte{0});
        payload[0] = std::byte{0xff}; payload[1] = std::byte{0xff};
        payload[2] = std::byte{0x00}; payload[3] = std::byte{0x38};
        write_u32(payload, 36, 4);
        const auto request = decode_parcel_object_owners_request(payload);
        if (!request || request->local_id != 4) return false;

        std::vector<ParcelObjectOwner> owners(1);
        owners[0].count = 12;
        owners[0].online = true;
        const auto reply = encode_parcel_object_owners_reply(owners);
        // prefix(4) + count(1) + one entry(16+1+4+1 = 22)
        if (reply.size() != 4 + 1 + 22 || reply[3] != std::byte{0x39} ||
            reply[4] != std::byte{1})
            return false;
    }
    // ParcelSelectObjects (Low 202) with one return id, and ForceObjectSelect encode.
    {
        std::vector<std::byte> payload(4 + 32 + 4 + 4 + 1 + 16, std::byte{0});
        payload[0] = std::byte{0xff}; payload[1] = std::byte{0xff};
        payload[2] = std::byte{0x00}; payload[3] = std::byte{0xca};
        write_u32(payload, 36, 6);   // local id
        write_u32(payload, 40, 8);   // return type = Other (1<<3)
        payload[44] = std::byte{1};  // one return id
        const auto select = decode_parcel_select_objects(payload);
        if (!select || select->local_id != 6 || select->return_type != 8 ||
            select->return_ids.size() != 1)
            return false;

        const std::array<std::uint32_t, 2> ids{7, 9};
        const auto packets = encode_force_object_select(ids);
        if (packets.size() != 1) return false;
        const auto& first = packets[0];
        // prefix(4) + reset(1) + count(1) + 2*4
        if (first.size() != 4 + 1 + 1 + 8 || first[3] != std::byte{0xcd} ||
            first[4] != std::byte{1} /*ResetList*/ || first[5] != std::byte{2} /*count*/)
            return false;
    }
    // ParcelReturnObjects (Low 199): return type + one task id + no owner ids.
    {
        std::vector<std::byte> payload(4 + 32 + 4 + 4 + 1 + 16 + 1, std::byte{0});
        payload[0] = std::byte{0xff}; payload[1] = std::byte{0xff};
        payload[2] = std::byte{0x00}; payload[3] = std::byte{0xc7};
        write_u32(payload, 36, 5);    // local id
        write_u32(payload, 40, 16);   // return type = List (1<<4)
        payload[44] = std::byte{1};   // one task id
        // owner id list count byte is at 44 + 1 + 16 = 61, left zero
        const auto ret = decode_parcel_return_objects(payload);
        if (!ret || ret->local_id != 5 || ret->return_type != 16 ||
            ret->task_ids.size() != 1 || !ret->owner_ids.empty())
            return false;
    }
    return true;
}

bool estate_codecs() {
    // RequestRegionInfo (Low 141).
    {
        std::vector<std::byte> payload(4 + 32, std::byte{0});
        payload[0] = std::byte{0xff}; payload[1] = std::byte{0xff};
        payload[2] = std::byte{0x00}; payload[3] = std::byte{0x8d};
        payload[4] = std::byte{0x11};
        const auto request = decode_request_region_info(payload);
        if (!request || std::to_integer<int>(request->agent_id[0]) != 0x11) return false;
    }
    // RegionInfo (Low 142) encode.
    {
        RegionInfoReply reply;
        reply.sim_name = "Region";
        reply.estate_id = 100;
        reply.region_flags = 6;
        const auto encoded = encode_region_info(reply);
        if (encoded.size() < 40 || encoded[0] != std::byte{0xff} || encoded[3] != std::byte{0x8e})
            return false;
    }
    // EstateOwnerMessage (Low 260) round-trip.
    {
        Uuid agent{};
        agent[0] = std::byte{0xab};
        Uuid invoice{};
        invoice[0] = std::byte{0xcd};
        const std::vector<std::string> params{"0", "64", "55555555-5555-4555-8555-555555555555"};
        const auto encoded = encode_estate_owner_message(agent, invoice, "estateaccessdelta", params);
        if (encoded[0] != std::byte{0xff} || encoded[2] != std::byte{0x01} || encoded[3] != std::byte{0x04})
            return false;
        const auto decoded = decode_estate_owner_message(encoded);
        if (!decoded || decoded->method != "estateaccessdelta" || decoded->params.size() != 3 ||
            decoded->params[1] != "64" ||
            decoded->params[2] != "55555555-5555-4555-8555-555555555555" ||
            std::to_integer<int>(decoded->agent_id[0]) != 0xab ||
            std::to_integer<int>(decoded->invoice[0]) != 0xcd)
            return false;
    }
    return true;
}

int main() {
    if (!packet_round_trip()) return 1;
    if (!parcel_codecs()) return 25;
    if (!estate_codecs()) return 26;
    if (!message_codecs()) return 2;
    if (!teleport_codecs()) return 18;
    if (!map_codecs()) return 17;
    if (!reliability()) return 3;
    if (!resend_throttle_and_timeout()) return 4;
    if (!circuit_registry()) return 5;
    if (!agent_update_codec()) return 6;
    if (!modify_land_codec()) return 13;
    if (!animation_codecs()) return 7;
    if (!wearable_asset_codecs()) return 8;
    if (!chat_codecs()) return 9;
    if (!flat_terrain_codec()) return 10;
    if (!extended_terrain_codec()) return 21;
    if (!static_object_codec()) return 11;
    if (!object_relationship_codecs()) return 19;
    if (!object_flag_codec()) return 14;
    if (!object_interaction_codecs()) return 15;
    if (!default_primitive_texture()) return 16;
    if (!task_inventory_codecs()) return 20;
    if (!transfer_codecs()) return 22;
    if (!baked_texture_entry_roundtrip()) return 23;
    if (!home_and_gesture_codecs()) return 24;
    if (decode_packet(std::array<std::byte, 2>{})) return 12;
    // Attachment messages, decoded against the layouts in message_template.msg
    // rather than against what seemed likely. Built here as real bytes so the
    // offsets are exercised, not just the field names.
    {
        std::vector<std::byte> wear{std::byte{0xff}, std::byte{0xff},
                                    std::byte{0x01}, std::byte{0x8b}};
        const auto push_uuid = [&](std::uint8_t fill) {
            for (int index = 0; index < 16; ++index) wear.push_back(std::byte{fill});
        };
        push_uuid(0xa1);                       // AgentID
        push_uuid(0xb2);                       // SessionID
        push_uuid(0xc3);                       // ItemID
        push_uuid(0xd4);                       // OwnerID
        wear.push_back(std::byte{0x85});       // AttachmentPt, with ATTACHMENT_ADD set
        for (int index = 0; index < 16; ++index) wear.push_back(std::byte{});  // four masks
        const std::string name = "SLReference";
        wear.push_back(static_cast<std::byte>(name.size() + 1));
        for (const char character : name) wear.push_back(static_cast<std::byte>(character));
        wear.push_back(std::byte{});           // the viewer's trailing NUL
        wear.push_back(std::byte{1});
        wear.push_back(std::byte{});           // empty description, NUL only

        const auto worn = homeworldz::viewer::decode_rez_single_attachment_from_inv(wear);
        if (!worn) return 27;
        if (worn->agent_id[0] != std::byte{0xa1} || worn->item_id[0] != std::byte{0xc3}) return 28;
        // The NUL the viewer appends is dropped: a name that does not compare
        // equal to the same name is found weeks later, in a search that fails.
        if (worn->name != "SLReference") return 29;
        if (!worn->description.empty()) return 30;
        // ATTACHMENT_ADD (0x80) rides on the point and is not part of it.
        if ((worn->attachment_point & 0x7f) != 5) return 31;
        // Truncation must be refused rather than read past the end.
        if (homeworldz::viewer::decode_rez_single_attachment_from_inv(
                std::span(wear).first(wear.size() - 1)))
            return 32;
    }
    {
        std::vector<std::byte> detach{std::byte{0xff}, std::byte{0xff},
                                      std::byte{0x00}, std::byte{0x71}};
        for (int index = 0; index < 32; ++index) detach.push_back(std::byte{0x11});
        detach.push_back(std::byte{2});        // a variable block: two local ids
        for (const std::uint32_t id : {std::uint32_t{258}, std::uint32_t{65538}})
            for (int shift = 0; shift < 32; shift += 8)
                detach.push_back(static_cast<std::byte>((id >> shift) & 0xff));
        const auto parsed = homeworldz::viewer::decode_object_detach(detach);
        if (!parsed) return 33;
        if (parsed->local_ids.size() != 2) return 34;
        if (parsed->local_ids[0] != 258 || parsed->local_ids[1] != 65538) return 35;
        // A count larger than the bytes that follow is malformed, not a short read.
        detach[36] = std::byte{9};
        if (homeworldz::viewer::decode_object_detach(detach)) return 36;
    }
    {
        // RezMultipleAttachmentsFromInv: the message a viewer actually sends.
        // Built to the layout of the viewer's own sender (llattachmentsmgr.cpp),
        // batched two objects to a packet the way it batches four.
        std::vector<std::byte> batch{std::byte{0xff}, std::byte{0xff},
                                     std::byte{0x01}, std::byte{0x8c}};
        const auto push_uuid = [&](std::uint8_t fill) {
            for (int index = 0; index < 16; ++index) batch.push_back(std::byte{fill});
        };
        const auto push_u32 = [&](std::uint32_t value) {
            for (int shift = 0; shift < 32; shift += 8)
                batch.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        };
        const auto push_string = [&](const std::string& text) {
            batch.push_back(static_cast<std::byte>(text.size() + 1));
            for (const char character : text) batch.push_back(static_cast<std::byte>(character));
            batch.push_back(std::byte{});   // the viewer's trailing NUL
        };
        push_uuid(0xa1);                    // AgentID
        push_uuid(0xb2);                    // SessionID
        push_uuid(0xc3);                    // CompoundMsgID
        batch.push_back(std::byte{2});      // TotalObjects
        batch.push_back(std::byte{1});      // FirstDetachAll: replace the outfit
        batch.push_back(std::byte{2});      // ObjectData count
        push_uuid(0xd4);                    // first ItemID
        push_uuid(0xe5);                    // first OwnerID
        batch.push_back(std::byte{0x85});   // point 5 with ATTACHMENT_ADD
        push_u32(5);                        // ItemFlags: the remembered point
        push_u32(0); push_u32(0); push_u32(0);   // the three cruft masks
        push_string("Left Hand Torch");
        push_string("burns");
        push_uuid(0xf6);                    // second ItemID
        push_uuid(0xe5);
        batch.push_back(std::byte{31});     // a point above 15, no ADD
        push_u32(0);
        push_u32(0); push_u32(0); push_u32(0);
        push_string("Hat");
        push_string("");

        const auto parsed = homeworldz::viewer::decode_rez_multiple_attachments_from_inv(batch);
        if (!parsed) return 41;
        if (parsed->agent_id[0] != std::byte{0xa1} ||
            parsed->compound_id[0] != std::byte{0xc3}) return 42;
        if (parsed->total_objects != 2 || !parsed->first_detach_all) return 43;
        if (parsed->objects.size() != 2) return 44;
        if (parsed->objects[0].item_id[0] != std::byte{0xd4} ||
            parsed->objects[0].attachment_point != 0x85 ||
            parsed->objects[0].item_flags != 5 ||
            parsed->objects[0].name != "Left Hand Torch" ||
            parsed->objects[0].description != "burns") return 45;
        // The second object is only reached by walking the first one's two
        // variable-length strings correctly. Its point is the assertion that
        // the walk landed where it should.
        if (parsed->objects[1].item_id[0] != std::byte{0xf6} ||
            parsed->objects[1].attachment_point != 31 ||
            parsed->objects[1].name != "Hat" ||
            !parsed->objects[1].description.empty()) return 46;
        // A batch cut short is malformed, not a shorter batch.
        if (homeworldz::viewer::decode_rez_multiple_attachments_from_inv(
                std::span(batch).first(batch.size() - 1))) return 47;
        // A count larger than the objects that follow must not read past them.
        auto overcounted = batch;
        overcounted[54] = std::byte{4};
        if (homeworldz::viewer::decode_rez_multiple_attachments_from_inv(overcounted)) return 48;
    }
    {
        // DetachAttachmentIntoInv carries no SessionID. Decoding must not
        // expect one, and 36 bytes is the whole message.
        std::vector<std::byte> detach{std::byte{0xff}, std::byte{0xff},
                                      std::byte{0x01}, std::byte{0x8d}};
        for (int index = 0; index < 16; ++index) detach.push_back(std::byte{0xa1});
        for (int index = 0; index < 16; ++index) detach.push_back(std::byte{0xd4});
        const auto parsed = homeworldz::viewer::decode_detach_attachment_into_inv(detach);
        if (!parsed) return 49;
        if (parsed->agent_id[0] != std::byte{0xa1} ||
            parsed->item_id[0] != std::byte{0xd4}) return 50;
        if (homeworldz::viewer::decode_detach_attachment_into_inv(
                std::span(detach).first(35))) return 51;
        // And it must not answer to its neighbours' message ids.
        auto wrong_id = detach;
        wrong_id[3] = std::byte{0x8c};
        if (homeworldz::viewer::decode_detach_attachment_into_inv(wrong_id)) return 52;
    }
    {
        // An attachment's ObjectUpdate has to carry AttachItemID, because that
        // is how the viewer tells two worn objects apart. Without it both read
        // as the same item and it detaches the older one — every Add replaced
        // instead of adding until this went in.
        homeworldz::viewer::StaticObject worn_object;
        worn_object.local_id = 42;
        worn_object.id = *homeworldz::viewer::parse_uuid(
            "af46ed87-9727-423a-bbf2-f2d81a451816");
        worn_object.owner_id = *homeworldz::viewer::parse_uuid(
            "efa3f54c-9be7-47c1-b6f3-197d778f32b3");
        worn_object.state = homeworldz::viewer::attachment_state(5);
        worn_object.attachment_item_id = *homeworldz::viewer::parse_uuid(
            "ad369719-9832-4128-ab07-d75d70322a41");
        const auto encoded = homeworldz::viewer::encode_static_object_update(1, worn_object);
        const std::string wire(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        if (wire.find("AttachItemID STRING RW SV ad369719-9832-4128-ab07-d75d70322a41\n") ==
            std::string::npos) return 53;
        // An ordinary prim carries no NameValue at all: a null AttachItemID on
        // something not worn is the collision this fixes, spelled differently.
        homeworldz::viewer::StaticObject ground_object = worn_object;
        ground_object.state = 0;
        ground_object.attachment_item_id = {};
        const auto plain = homeworldz::viewer::encode_static_object_update(1, ground_object);
        const std::string plain_wire(reinterpret_cast<const char*>(plain.data()), plain.size());
        if (plain_wire.find("AttachItemID") != std::string::npos) return 54;
    }
    {
        // The State byte carries the attachment point with its nibbles swapped.
        // The check is against the viewer's own expression, written out here, and
        // not against what this region happens to produce.
        const auto viewer_reads = [](std::uint8_t state) {
            return static_cast<std::uint8_t>(((state & 0xf0U) >> 4) | ((state & 0x0fU) << 4));
        };
        // Right hand: the point a plain Wear falls back to.
        if (homeworldz::viewer::attachment_state(5) != 0x50) return 37;
        // Points run past 15, so the swap has to carry both nibbles: a byte-wide
        // shift would lose every HUD point and every Bento point above chest.
        if (homeworldz::viewer::attachment_state(31) != 0xf1) return 38;
        for (std::uint8_t point = 0; point < 0x80; ++point)
            if (viewer_reads(homeworldz::viewer::attachment_state(point)) != point) return 39;
        // Zero means "not an attachment" and must stay zero, or every ordinary
        // prim in the region claims to be worn on point 0.
        if (homeworldz::viewer::attachment_state(0) != 0) return 40;
    }

    return 0;
}
