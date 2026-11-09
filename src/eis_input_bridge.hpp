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
class InputState;

class EisInputBridge : public QObject {
    Q_OBJECT

  public:
    explicit EisInputBridge(WaylandInput& input, InputState& inputs, QObject* parent = nullptr);
    ~EisInputBridge() override;

    EisInputBridge(const EisInputBridge&) = delete;
    EisInputBridge& operator=(const EisInputBridge&) = delete;

    [[nodiscard]] std::optional<int> addClient(const QString& sessionId, std::uint32_t deviceTypes, QString* errorText = nullptr);

    void removeClient(const QString& sessionId);

  private:
    struct ClientContext {
        eis* context = nullptr;
        std::uint32_t deviceTypes = 0;
        QObject* notifier = nullptr;
    };

    struct SeatState {
        eis_client* client = nullptr;
        std::uint32_t deviceTypes = 0;
        eis_device* keyboard = nullptr;
        eis_device* text = nullptr;
        eis_device* pointer = nullptr;
        eis_device* absolutePointer = nullptr;
    };

    void dispatch(eis* context);
    void handleEvent(eis_event* event);
    void handleClientConnect(eis_client* client);
    void handleClientDisconnect(eis_client* client);
    void handleSeatBind(eis_event* event);
    void handleDeviceClosed(eis_device* device);
    void handleInputEvent(eis_event* event);
    void releaseInput(eis_device* device);
    eis_device* addKeyboard(eis_seat* seat);
    eis_device* addText(eis_seat* seat);
    eis_device* addPointer(eis_seat* seat);
    eis_device* addAbsolutePointer(eis_seat* seat);
    void removeSeat(eis_seat* seat);
    void cleanup();

    WaylandInput& m_input;
    InputState& m_inputs;
    QHash<QString, ClientContext> m_clients;
    QHash<eis_seat*, SeatState*> m_seats;
    QHash<eis_device*, QPair<int, int>> m_scrollRemainders;
};

} // namespace hkcf
