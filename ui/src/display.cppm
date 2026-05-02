module;
#include "QExtra/macro_qt.hpp"

#ifdef Q_MOC_RUN
#    include "waywallen/display.moc"
#endif

export module waywallen:display;
export import :proto;
export import :backend;
import rstd;
import rstd.cppstd;
import qextra;

using rstd::boxed::Box;

namespace proto = waywallen::control::v1;

export namespace waywallen
{

/// One display, mirroring `proto::DisplayInfo` as a QObject so QML can
/// bind directly to its fields. Identity is `id()`; mutate via
/// `updateFrom(info)` which diff-emits per changed property.
class Display : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Display instances are owned by DisplayManager")

    Q_PROPERTY(quint64 id READ id CONSTANT FINAL)
    Q_PROPERTY(QString name READ name NOTIFY nameChanged FINAL)
    Q_PROPERTY(quint32 width READ width NOTIFY sizeChanged FINAL)
    Q_PROPERTY(quint32 height READ height NOTIFY sizeChanged FINAL)
    Q_PROPERTY(quint32 refreshMhz READ refreshMhz NOTIFY refreshMhzChanged FINAL)
    Q_PROPERTY(QVariantList links READ links NOTIFY linksChanged FINAL)
    /// Resolved layout currently in use for this display
    /// (per-display override on top of global defaults). Map keys:
    /// `fillmode` (int), `align` (int), `clearRgba` (QVariantList<float>, length 4).
    Q_PROPERTY(QVariantMap effectiveLayout READ effectiveLayout NOTIFY layoutChanged FINAL)
    /// Sparse per-display override. Same key set as effectiveLayout
    /// plus `fillmodeSet` / `alignSet` / `clearRgbaSet` booleans
    /// indicating whether each field is explicitly overridden vs. inherited.
    Q_PROPERTY(QVariantMap layoutOverride READ layoutOverride NOTIFY layoutChanged FINAL)

public:
    explicit Display(const proto::DisplayInfo& info, QObject* parent = nullptr);

    auto id() const -> quint64 { return m_id; }
    auto name() const -> const QString& { return m_name; }
    auto width() const -> quint32 { return m_width; }
    auto height() const -> quint32 { return m_height; }
    auto refreshMhz() const -> quint32 { return m_refresh_mhz; }
    auto links() const -> const QVariantList& { return m_links; }
    auto effectiveLayout() const -> const QVariantMap& { return m_effective_layout; }
    auto layoutOverride() const -> const QVariantMap& { return m_layout_override; }

    /// Diff-update from a freshly-received `DisplayInfo`. Only emits
    /// the signals for properties that actually changed.
    void updateFrom(const proto::DisplayInfo& info);

    Q_SIGNAL void nameChanged();
    Q_SIGNAL void sizeChanged();
    Q_SIGNAL void refreshMhzChanged();
    Q_SIGNAL void linksChanged();
    Q_SIGNAL void layoutChanged();

private:
    static auto linksFromPb(const proto::DisplayInfo& info) -> QVariantList;
    static auto effectiveLayoutFromPb(const proto::DisplayInfo& info) -> QVariantMap;
    static auto layoutOverrideFromPb(const proto::DisplayInfo& info) -> QVariantMap;

    quint64      m_id;
    QString      m_name;
    quint32      m_width;
    quint32      m_height;
    quint32      m_refresh_mhz;
    QVariantList m_links;
    QVariantMap  m_effective_layout;
    QVariantMap  m_layout_override;
};

/// Singleton model for all currently-registered displays. Fed by:
///   1. the snapshot that arrives on ws connect (via `Backend::eventReceived`),
///   2. subsequent `DisplayChanged` / `DisplayRemoved` events,
///   3. `DisplayListQuery::reload` as a fallback refresh path.
///
/// Consumers should prefer reading from `DisplayManager` over issuing
/// a fresh `DisplayListRequest` — the manager is push-updated.
class DisplayManager : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QVariantList displays READ displays NOTIFY displaysChanged FINAL)
    Q_PROPERTY(int count READ count NOTIFY displaysChanged FINAL)

public:
    DisplayManager(QObject* parent = nullptr);
    ~DisplayManager() override;

    static auto instance() -> DisplayManager*;

    /// Snapshot of all displays (ordered by ascending id) as a list of
    /// `Display*`, suitable for QML `Repeater { model: DisplayManager.displays }`.
    auto displays() const -> QVariantList;
    auto count() const -> int { return (int)m_ordered.size(); }

    Q_INVOKABLE waywallen::Display* get(quint64 id) const;

    /// Full replace. Removes any id not present in `list`, upserts the rest.
    /// Exactly-once `displaysChanged` after the batch.
    void replaceAll(const QList<proto::DisplayInfo>& list);

    /// Upsert a single display; emits `displaysChanged` only if this
    /// was an add (removal/add changes the ordered list). Property
    /// changes on an existing display emit per-property signals.
    void upsert(const proto::DisplayInfo& info);

    /// Remove by id. Emits `displaysChanged` if the id existed.
    void remove(quint64 id);

    /// Wire up to a backend's `eventReceived` signal. Call once from
    /// `App::init` after the backend is constructed.
    void attachTo(Backend* backend);

    Q_SIGNAL void displaysChanged();

private:
    void handleEvent(const proto::Event& evt);

    QList<Display*>               m_ordered;  // sorted by id
    std::map<quint64, Display*> m_by_id;
};

} // namespace waywallen
