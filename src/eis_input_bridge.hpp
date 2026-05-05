#pragma once

#include <optional>

#include <QObject>
#include <QHash>
#include <QString>

struct eis;
struct eis_client;
struct eis_device;
struct eis_event;
struct eis_seat;

namespace hkcf {

class WaylandInput;

class EisInputBridge : public QObject {
    Q_OBJECT

  public:
    explicit EisInputBridge(WaylandInput& input, QObject* parent = nullptr);
    ~EisInputBridge() override;

    EisInputBridge(const EisInputBridge&) = delete;
    EisInputBridge& operator=(const EisInputBridge&) = delete;

    [[nodiscard]] std::optional<int> addClient(QString* errorText = nullptr);

  private:
    struct SeatState {
        eis_client* client = nullptr;
        eis_device* keyboard = nullptr;
        eis_device* pointer = nullptr;
        eis_device* absolutePointer = nullptr;
    };

    [[nodiscard]] bool ensureContext(QString* errorText);
    void dispatch();
    void handleEvent(eis_event* event);
    void handleClientConnect(eis_client* client);
    void handleClientDisconnect(eis_client* client);
    void handleSeatBind(eis_event* event);
    void handleDeviceClosed(eis_device* device);
    void handleInputEvent(eis_event* event);
    eis_device* addKeyboard(eis_seat* seat);
    eis_device* addPointer(eis_seat* seat);
    eis_device* addAbsolutePointer(eis_seat* seat);
    void removeSeat(eis_seat* seat);
    void cleanup();

    WaylandInput& m_input;
    eis* m_eis = nullptr;
    QObject* m_notifier = nullptr;
    QHash<eis_seat*, SeatState*> m_seats;
};

} // namespace hkcf
