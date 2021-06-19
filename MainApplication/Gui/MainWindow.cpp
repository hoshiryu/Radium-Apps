#include <Gui/MainWindow.hpp>
#include <MainApplication.hpp>

#include <Core/Asset/FileLoaderInterface.hpp>
#include <Engine/Entity/Entity.hpp>
#include <Engine/Managers/EntityManager/EntityManager.hpp>
#include <Engine/Managers/SignalManager/SignalManager.hpp>
#include <Engine/Renderer/Material/Material.hpp>
#include <Engine/Renderer/Material/MaterialConverters.hpp>
#include <Engine/Renderer/Mesh/Mesh.hpp>
#include <Engine/Renderer/RenderObject/RenderObject.hpp>
#include <Engine/Renderer/RenderObject/RenderObjectManager.hpp>
#include <Engine/Renderer/Renderers/ForwardRenderer.hpp>
#include <GuiBase/Timeline/Timeline.hpp>
#include <GuiBase/TreeModel/EntityTreeModel.hpp>
#include <GuiBase/Utils/KeyMappingManager.hpp>
#include <GuiBase/Utils/qt_utils.hpp>
#include <GuiBase/Viewer/FlightCameraManipulator.hpp>
#include <GuiBase/Viewer/Gizmo/GizmoManager.hpp>
#include <GuiBase/Viewer/TrackballCameraManipulator.hpp>
#include <GuiBase/Viewer/Viewer.hpp>
#include <IO/deprecated/OBJFileManager.hpp>
#include <PluginBase/RadiumPluginInterface.hpp>

#include <Core/Utils/StringUtils.hpp>
#include <Engine/Managers/SystemDisplay/SystemDisplay.hpp>
#include <Engine/Renderer/Camera/Camera.hpp>

#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>

#include <mdd_file_loader/endianess.hpp>
#include <mdd_file_loader/point_cache_export.hpp>

using Ra::Engine::ItemEntry;

namespace Ra {
namespace Gui {

using namespace Core::Utils; // log

MainWindow::MainWindow( QWidget* parent ) : MainWindowInterface( parent ) {
    // Note : at this point most of the components (including the Engine) are
    // not initialized. Listen to the "started" signal.

    setupUi( this );

    m_viewer = new Viewer();
    // Registers the application dependant camera manipulators
    auto keyMappingManager = Gui::KeyMappingManager::getInstance();
    keyMappingManager->addListener( Gui::FlightCameraManipulator::configureKeyMapping );

    connect( m_viewer, &Viewer::glInitialized, this, &MainWindow::onGLInitialized );
    connect( m_viewer, &Viewer::rendererReady, this, &MainWindow::onRendererReady );

    m_viewer->setObjectName( QStringLiteral( "m_viewer" ) );

    QWidget* viewerwidget = QWidget::createWindowContainer( m_viewer );
    //  viewerwidget->setMinimumSize( QSize( 800, 600 ) );
    viewerwidget->setAutoFillBackground( false );

    setCentralWidget( viewerwidget );

    // Register the timeline
    m_timeline = new Ra::GuiBase::Timeline( this );
    m_timeline->onChangeEnd( Ra::Engine::RadiumEngine::getInstance()->getEndTime() );
    dockWidget_2->setWidget( m_timeline );
    
    setWindowIcon( QPixmap( ":/Resources/Icons/RadiumIcon.png" ) );
    setWindowTitle( QString( "Radium Engine" ) );

    QStringList headers;
    headers << tr( "Entities -> Components" );
    m_itemModel = new GuiBase::ItemModel( mainApp->getEngine(), this );
    m_entitiesTreeView->setModel( m_itemModel );
    m_materialEditor   = std::make_unique<MaterialEditor>();
    m_selectionManager = new GuiBase::SelectionManager( m_itemModel, this );
    m_entitiesTreeView->setSelectionModel( m_selectionManager );

    createConnections();

    mainApp->framesCountForStatsChanged( uint( m_avgFramesCount->value() ) );

    // load default color from QSettings
    updateBackgroundColor();

    asset::endianess::init();
}

MainWindow::~MainWindow() {
    // Child QObjects will automatically be deleted
}

void MainWindow::cleanup() {
    m_viewer->getGizmoManager()->cleanup();
}

void MainWindow::activateTrackballManipulator() {
    // set trackball manipulator (default)
    m_viewer->setCameraManipulator(
        new Gui::TrackballCameraManipulator( *( m_viewer->getCameraManipulator() ) ) );
}

void MainWindow::activateFlightManipulator() {
    // set flightmode manipulator
    m_viewer->setCameraManipulator(
        new Gui::FlightCameraManipulator( *( m_viewer->getCameraManipulator() ) ) );
}

// Connection to gizmos must be done after GL is initialized
void MainWindow::createConnections() {
    connect( actionOpenMesh, &QAction::triggered, this, &MainWindow::loadFile );
    connect( actionReload_Shaders, &QAction::triggered, m_viewer, &Viewer::reloadShaders );
    connect(
        actionOpen_Material_Editor, &QAction::triggered, this, &MainWindow::openMaterialEditor );

    connect( actionFlight, &QAction::triggered, this, &MainWindow::activateFlightManipulator );
    connect(
        actionTrackball, &QAction::triggered, this, &MainWindow::activateTrackballManipulator );
    connect( actionAdd_plugin_path, &QAction::triggered, this, &MainWindow::addPluginPath );
    connect( actionClear_plugin_paths, &QAction::triggered, this, &MainWindow::clearPluginPaths );

    // Toolbox setup
    // to update display when mode is changed
    connect( actionToggle_Local_Global,
             &QAction::toggled,
             mainApp,
             &Ra::GuiBase::BaseApplication::askForUpdate );

    connect( actionGizmoOff, &QAction::triggered, this, &MainWindow::gizmoShowNone );
    connect( actionGizmoTranslate, &QAction::triggered, this, &MainWindow::gizmoShowTranslate );
    connect( actionGizmoRotate, &QAction::triggered, this, &MainWindow::gizmoShowRotate );
    connect( actionGizmoScale, &QAction::triggered, this, &MainWindow::gizmoShowScale );

    connect( actionSnapshot, &QAction::triggered, mainApp, &MainApplication::recordFrame );
    connect( actionRecord_Frames, &QAction::toggled, mainApp, &MainApplication::setRecordFrames );

    connect(
        actionReload_configuration, &QAction::triggered, this, &MainWindow::reloadConfiguration );
    connect(
        actionLoad_configuration_file, &QAction::triggered, this, &MainWindow::loadConfiguration );

    // Timeline setup
    connect( m_timeline, &Ra::GuiBase::Timeline::playClicked, this, &MainWindow::timelinePlay );
    connect(
        m_timeline, &Ra::GuiBase::Timeline::cursorChanged, this, &MainWindow::timelineGoTo );
    connect( m_timeline,
             &Ra::GuiBase::Timeline::startChanged,
             this,
             &MainWindow::timelineStartChanged );
    connect(
        m_timeline, &Ra::GuiBase::Timeline::endChanged, this, &MainWindow::timelineEndChanged );
    connect( m_timeline,
             &Ra::GuiBase::Timeline::setPingPong,
             this,
             &MainWindow::timelineSetPingPong );
    connect( m_timeline, &Ra::GuiBase::Timeline::keyFrameChanged, [=]( Scalar ) {
        mainApp->askForUpdate();
    } );

    // Loading setup.
    connect( this, &MainWindow::fileLoading, mainApp, &Ra::GuiBase::BaseApplication::loadFile );

    // Connect picking results (TODO Val : use events to dispatch picking directly)
    connect( m_viewer, &Viewer::toggleBrushPicking, this, &MainWindow::toggleCirclePicking );
    connect( m_viewer, &Viewer::rightClickPicking, this, &MainWindow::handlePicking );
    // leftClickPicking is obsolete with the new input manager

    connect( m_avgFramesCount,
             static_cast<void ( QSpinBox::* )( int )>( &QSpinBox::valueChanged ),
             mainApp,
             &Ra::GuiBase::BaseApplication::framesCountForStatsChanged );
    connect( mainApp,
             &Ra::GuiBase::BaseApplication::updateFrameStats,
             this,
             &MainWindow::onUpdateFramestats );

    // Inform property editors of new selections
    connect( m_selectionManager,
             &GuiBase::SelectionManager::selectionChanged,
             this,
             &MainWindow::onSelectionChanged );
    // connect(this, &MainWindow::selectedItem, tab_edition, &TransformEditorWidget::setEditable);

    // Make selected item event visible to plugins
    connect( this, &MainWindow::selectedItem, mainApp, &MainApplication::onSelectedItem );

    // Enable changing shaders
    connect(
        m_currentShaderBox,
        static_cast<void ( QComboBox::* )( const QString& )>( &QComboBox::currentIndexChanged ),
        this,
        &MainWindow::changeRenderObjectShader );

    // RO Stuff
    connect(
        m_itemModel, &GuiBase::ItemModel::visibilityROChanged, this, &MainWindow::setROVisible );
    connect( m_editRenderObjectButton, &QPushButton::clicked, this, &MainWindow::editRO );
    connect( m_exportMeshButton, &QPushButton::clicked, this, &MainWindow::exportCurrentMesh );
    connect( m_removeEntityButton, &QPushButton::clicked, this, &MainWindow::deleteCurrentItem );
    connect( m_clearSceneButton, &QPushButton::clicked, this, &MainWindow::resetScene );
    connect( m_fitCameraButton, &QPushButton::clicked, this, &MainWindow::fitCamera );
    connect( m_showHideAllButton, &QPushButton::clicked, this, &MainWindow::showHideAllRO );
    connect( m_exportMeshEveryFrame, &QPushButton::toggled, this, &MainWindow::exportMeshEveryFrame );

    // Renderer stuff
    connect(
        m_currentRendererCombo,
        static_cast<void ( QComboBox::* )( const QString& )>( &QComboBox::currentIndexChanged ),
        [=]( const QString& ) { this->onCurrentRenderChangedInUI(); } );

    connect(
        m_displayedTextureCombo,
        static_cast<void ( QComboBox::* )( const QString& )>( &QComboBox::currentIndexChanged ),
        m_viewer,
        &Viewer::displayTexture );

    connect( m_enablePostProcess, &QCheckBox::stateChanged, m_viewer, &Viewer::enablePostProcess );
    connect( m_enableDebugDraw, &QCheckBox::stateChanged, m_viewer, &Viewer::enableDebugDraw );
    connect( m_realFrameRate,
             &QCheckBox::stateChanged,
             mainApp,
             &Ra::GuiBase::BaseApplication::setRealFrameRate );

    connect( m_printGraph,
             &QCheckBox::stateChanged,
             mainApp,
             &Ra::GuiBase::BaseApplication::setRecordGraph );
    connect( m_printTimings,
             &QCheckBox::stateChanged,
             mainApp,
             &Ra::GuiBase::BaseApplication::setRecordTimings );

    // Material editor
    connect( m_materialEditor.get(),
             &MaterialEditor::materialChanged,
             mainApp,
             &Ra::GuiBase::BaseApplication::askForUpdate );

    // Connect engine signals to the appropriate callbacks
    std::function<void( const Engine::ItemEntry& )> add =
        std::bind( &MainWindow::onItemAdded, this, std::placeholders::_1 );
    std::function<void( const Engine::ItemEntry& )> del =
        std::bind( &MainWindow::onItemRemoved, this, std::placeholders::_1 );
    mainApp->m_engine->getSignalManager()->m_entityCreatedCallbacks.push_back( add );
    mainApp->m_engine->getSignalManager()->m_entityDestroyedCallbacks.push_back( del );

    mainApp->m_engine->getSignalManager()->m_componentAddedCallbacks.push_back( add );
    mainApp->m_engine->getSignalManager()->m_componentRemovedCallbacks.push_back( del );

    mainApp->m_engine->getSignalManager()->m_roAddedCallbacks.push_back( add );
    mainApp->m_engine->getSignalManager()->m_roRemovedCallbacks.push_back( del );
}

void MainWindow::loadFile() {

    QString filter;

    QString allexts;
    for ( const auto& loader : mainApp->m_engine->getFileLoaders() )
    {
        QString exts;
        for ( const auto& e : loader->getFileExtensions() )
        {
            exts.append( QString::fromStdString( e ) + tr( " " ) );
        }
        allexts.append( exts + tr( " " ) );
        filter.append( QString::fromStdString( loader->name() ) + tr( " (" ) + exts + tr( ");;" ) );
    }
    // add a filter concetenatting all the supported extensions
    filter.prepend( tr( "Supported files (" ) + allexts + tr( ");;" ) );

    // remove the last ";;" of the string
    filter.remove( filter.size() - 2, 2 );

    QSettings settings;
    QString path         = settings.value( "files/load", QDir::homePath() ).toString();
    QStringList pathList = QFileDialog::getOpenFileNames( this, "Open Files", path, filter );

    if ( !pathList.empty() )
    {
        settings.setValue( "files/load", pathList.front() );

        for ( const auto& file : pathList )
        {
            emit fileLoading( file );
        }
    }
}

void MainWindow::onUpdateFramestats( const std::vector<FrameTimerData>& stats ) {
    QString framesA2B = QString( "Frames #%1 to #%2 stats :" )
                            .arg( stats.front().numFrame )
                            .arg( stats.back().numFrame );
    m_frameA2BLabel->setText( framesA2B );

    auto romgr = mainApp->m_engine->getRenderObjectManager();

    auto polycount   = romgr->getNumFaces();
    auto vertexcount = romgr->getNumVertices();

    QString polyCountText =
        QString( "Rendering %1 faces and %2 vertices" ).arg( polycount ).arg( vertexcount );
    m_labelCount->setText( polyCountText );

    long sumRender     = 0;
    long sumTasks      = 0;
    long sumFrame      = 0;
    long sumInterFrame = 0;

    for ( uint i = 0; i < stats.size(); ++i )
    {
        sumRender += Core::Utils::getIntervalMicro( stats[i].renderData.renderStart,
                                                    stats[i].renderData.renderEnd );
        sumTasks += Core::Utils::getIntervalMicro( stats[i].tasksStart, stats[i].tasksEnd );
        sumFrame += Core::Utils::getIntervalMicro( stats[i].frameStart, stats[i].frameEnd );

        if ( i > 0 )
        {
            sumInterFrame +=
                Core::Utils::getIntervalMicro( stats[i - 1].frameEnd, stats[i].frameEnd );
        }
    }

    const uint N{uint( stats.size() )};
    const Scalar T( N * 1000000.f );
    m_renderTime->setNum( int( sumRender / N ) );
    m_renderUpdates->setNum( int( T / Scalar( sumRender ) ) );
    m_tasksTime->setNum( int( sumTasks / N ) );
    m_tasksUpdates->setNum( int( T / Scalar( sumTasks ) ) );
    m_frameTime->setNum( int( sumFrame / N ) );
    m_frameUpdates->setNum( int( T / Scalar( sumFrame ) ) );
    m_avgFramerate->setNum( int( ( N - 1 ) * Scalar( 1000000.0 / sumInterFrame ) ) );
}

Viewer* MainWindow::getViewer() {
    return m_viewer;
}

GuiBase::SelectionManager* MainWindow::getSelectionManager() {
    return m_selectionManager;
}

GuiBase::Timeline* MainWindow::getTimeline() {
    return m_timeline;
}

void Gui::MainWindow::toggleCirclePicking( bool on ) {
    centralWidget()->setMouseTracking( on );
}

void MainWindow::handlePicking( const Engine::Renderer::PickingResult& pickingResult ) {
    Ra::Core::Utils::Index roIndex( pickingResult.m_roIdx );
    Ra::Engine::RadiumEngine* engine = Ra::Engine::RadiumEngine::getInstance();
    if ( roIndex.isValid() )
    {
        auto ro = engine->getRenderObjectManager()->getRenderObject( roIndex );
        if ( ro->getType() != Ra::Engine::RenderObjectType::UI )
        {
            Ra::Engine::Component* comp = ro->getComponent();
            Ra::Engine::Entity* ent     = comp->getEntity();

            // For now we don't enable group selection.
            m_selectionManager->setCurrentEntry( ItemEntry( ent, comp, roIndex ),
                                                 QItemSelectionModel::ClearAndSelect |
                                                     QItemSelectionModel::Current );
        }
    }
    else
    { m_selectionManager->clear(); }
}

void MainWindow::onSelectionChanged( const QItemSelection& /*selected*/,
                                     const QItemSelection& /*deselected*/ ) {
    m_currentShaderBox->setEnabled( false );

    if ( m_selectionManager->hasSelection() )
    {
        const ItemEntry& ent = m_selectionManager->currentItem();
        emit selectedItem( ent );
        m_selectedItemName->setText(
            QString::fromStdString( getEntryName( mainApp->getEngine(), ent ) ) );
        m_editRenderObjectButton->setEnabled( false );

        if ( ent.isRoNode() )
        {
            m_editRenderObjectButton->setEnabled( true );

//            m_materialEditor->changeRenderObject( ent.m_roIndex );
//            auto material = mainApp->m_engine->getRenderObjectManager()
//                                ->getRenderObject( ent.m_roIndex )
//                                ->getMaterial();
//            const std::string& shaderName = material->getMaterialName();
//            CORE_ASSERT( m_currentShaderBox->findText( shaderName.c_str() ) != -1,
//                         "RO shaders must be already added to the list" );
//            m_currentShaderBox->setCurrentText( shaderName.c_str() );
            // m_currentShaderBox->setEnabled( true ); // commented out, as there is no simple way
            // to change the material type
        }
        else
            { m_currentShaderBox->setCurrentText( "" ); }
        m_timeline->selectionChanged( ent );
    }
    else
    {
        m_currentShaderBox->setCurrentText( "" );
        emit selectedItem( ItemEntry() );
        m_selectedItemName->setText( "" );
        m_editRenderObjectButton->setEnabled( false );
        m_materialEditor->hide();
        m_timeline->selectionChanged( ItemEntry() );
    }
}

void MainWindow::closeEvent( QCloseEvent* event ) {
    emit closed();
    event->accept();
}

void MainWindow::gizmoShowNone() {
    m_viewer->getGizmoManager()->changeGizmoType( GizmoManager::NONE );
    mainApp->askForUpdate();
}

void MainWindow::gizmoShowTranslate() {
    m_viewer->getGizmoManager()->changeGizmoType( GizmoManager::TRANSLATION );
    mainApp->askForUpdate();
}

void MainWindow::gizmoShowRotate() {
    m_viewer->getGizmoManager()->changeGizmoType( GizmoManager::ROTATION );
    mainApp->askForUpdate();
}

void MainWindow::gizmoShowScale() {
    m_viewer->getGizmoManager()->changeGizmoType( GizmoManager::SCALE );
    mainApp->askForUpdate();
}

void MainWindow::reloadConfiguration() {
    KeyMappingManager::getInstance()->reloadConfiguration();
}

void MainWindow::loadConfiguration() {
    QSettings settings;
    QString path = settings.value( "configs/load", QDir::homePath() ).toString();
    path         = QFileDialog::getOpenFileName(
        this, "Open Configuration File", path, "Configuration file (*.xml)" );

    if ( path.size() > 0 )
    {
        settings.setValue( "configs/load", path );
        KeyMappingManager::getInstance()->loadConfiguration( path.toStdString().c_str() );
    }
}

void MainWindow::onCurrentRenderChangedInUI() {
    // always restore displaytexture to 0 before switch to keep coherent renderer state
    m_displayedTextureCombo->setCurrentIndex( 0 );
    if ( m_viewer->changeRenderer( m_currentRendererCombo->currentIndex() ) )
    {
        updateDisplayedTexture();
        // in case the newly used renderer has not been set before and set another texture as its
        // default, set displayTexture to 0 again ;)
        m_displayedTextureCombo->setCurrentIndex( 0 );
    }
}

void MainWindow::updateDisplayedTexture() {
    QSignalBlocker blockTextures( m_displayedTextureCombo );

    m_displayedTextureCombo->clear();

    auto texs = m_viewer->getRenderer()->getAvailableTextures();
    for ( const auto& tex : texs )
    {
        m_displayedTextureCombo->addItem( tex.c_str() );
    }
}

void MainWindow::updateBackgroundColor( QColor c ) {
    // FIXME : sometime, settings does not define colrs but Qt found one ....
    QSettings settings;
    // Get or set color from/to settings
    if ( !c.isValid() )
    {
        // get the default color or an already existing one
        auto defColor = Core::Utils::Color::linearRGBTosRGB( m_viewer->getBackgroundColor() );
        auto bgk      = QColor::fromRgb(
            defColor.rgb()[0] * 255, defColor.rgb()[1] * 255, defColor.rgb()[2] * 255 );
        c = settings.value( "colors/background", bgk ).value<QColor>();
    }
    else
    { settings.setValue( "colors/background", c ); }

    // update the color of the button
    QString qss = QString( "background-color: %1" ).arg( c.name() );
    m_currentColorButton->setStyleSheet( qss );

    // update the background coolor of the viewer
    auto bgk = Core::Utils::Color::sRGBToLinearRGB( Core::Utils::Color(
        Scalar( c.redF() ), Scalar( c.greenF() ), Scalar( c.blueF() ), Scalar( 0 ) ) );
    m_viewer->setBackgroundColor( bgk );
}

// Is this still a useful feature ?
void MainWindow::changeRenderObjectShader( const QString& shaderName ) {
    // FIXME : is this still a wanted feature. Commented out for now. if this feature is wanted,
    // need to find a
    //  way to change the render-technique.
    /*
        std::string name = shaderName.toStdString();
        if ( name.empty() ) { return; }

        const ItemEntry& item = m_selectionManager->currentItem();
        const Engine::ShaderConfiguration config =
            Ra::Engine::ShaderConfigurationFactory::getConfiguration( name );

        auto vector_of_ros = getItemROs( mainApp->m_engine.get(), item );
        for ( const auto& ro_index : vector_of_ros )
        {
            const auto& ro = mainApp->m_engine->getRenderObjectManager()->getRenderObject( ro_index
       ); if ( ro->getRenderTechnique()->getMaterial()->getMaterialName() != name )
            {
                // FIXME: this changes only the render technique, not the associated shader.

                auto builder = Ra::Engine::EngineRenderTechniques::getDefaultTechnique( name );
                builder.second( *ro->getRenderTechnique().get(), false );
            }
        }
    */
}

void Gui::MainWindow::setROVisible( Core::Utils::Index roIndex, bool visible ) {
    mainApp->m_engine->getRenderObjectManager()->getRenderObject( roIndex )->setVisible( visible );
    mainApp->askForUpdate();
}

void Gui::MainWindow::editRO() {
    ItemEntry item = m_selectionManager->currentItem();
    if ( item.isRoNode() )
    {
        m_materialEditor->changeRenderObject( item.m_roIndex );
        m_materialEditor->show();
    }
}

void Gui::MainWindow::showHideAllRO() {
    bool allEntityInvisible = true;

    const int j = 0;
    for ( int i = 0; i < m_itemModel->rowCount(); ++i )
    {
        auto idx  = m_itemModel->index( i, j );
        auto item = m_itemModel->getEntry( idx );
        if ( item.isValid() && item.isSelectable() )
        {
            bool isVisible = m_itemModel->data( idx, Qt::CheckStateRole ).toBool();
            if ( isVisible )
            {
                allEntityInvisible = false;
                break;
            }
        }
    }

    // if all entities are invisible : show all
    // if at least one entity is visible : hide all
    for ( int i = 0; i < m_itemModel->rowCount(); ++i )
    {
        auto idx  = m_itemModel->index( i, j );
        auto item = m_itemModel->getEntry( idx );
        if ( item.isValid() && item.isSelectable() )
        { m_itemModel->setData( idx, allEntityInvisible, Qt::CheckStateRole ); }
    }
    mainApp->askForUpdate();
}

void Gui::MainWindow::openMaterialEditor() {
    m_materialEditor->show();
}

void Gui::MainWindow::updateUi( Plugins::RadiumPluginInterface* plugin ) {
    QString tabName;

    // Add menu
    if ( plugin->doAddMenu() ) { QMainWindow::menuBar()->addMenu( plugin->getMenu() ); }

    // Add widget
    if ( plugin->doAddWidget( tabName ) ) { toolBox->addTab( plugin->getWidget(), tabName ); }

    // Add actions
    int nbActions;
    if ( plugin->doAddAction( nbActions ) )
    {
        for ( int i = 0; i < nbActions; ++i )
        {
            toolBar->insertAction( nullptr, plugin->getAction( i ) );
        }
        toolBar->addSeparator();
    }
}

void MainWindow::onRendererReady() {
    updateDisplayedTexture();
}

bool findDuplicates( const Ra::Core::Geometry::TriangleMesh& mesh, std::vector<Index>& duplicatesMap ) {
    bool hasDuplicates = false;
    duplicatesMap.clear();
    const size_t numVerts = mesh.vertices().size();
    duplicatesMap.resize( numVerts, Index::Invalid() );

    std::vector<std::pair<Ra::Core::Vector3, Index>> vertices( numVerts );

#pragma omp parallel for schedule( static )
    for ( int i = 0; i < int( numVerts ); ++i )
    {
        vertices[uint( i )] = std::make_pair( mesh.vertices()[uint( i )], Index( i ) );
    }

    std::sort( vertices.begin(), vertices.end(), []( const auto& a, const auto& b ) {
        if ( a.first.x() == b.first.x() )
        {
            if ( a.first.y() == b.first.y() )
                if ( a.first.z() == b.first.z() )
                    return a.second < b.second;
                else
                    return a.first.z() < b.first.z();
            else
                return a.first.y() < b.first.y();
        }
        return a.first.x() < b.first.x();
    } );

    // Here vertices contains vertex pos and idx, with equal
    // vertices contiguous, sorted by idx, so checking if current
    // vertex equals the previous one state if its a duplicated
    // vertex position.
    duplicatesMap[uint( vertices[0].second )] = vertices[0].second;
    for ( uint i = 1; i < numVerts; ++i )
    {
        if ( vertices[i].first == vertices[i - 1].first )
        {
            duplicatesMap[uint( vertices[i].second )] =
                duplicatesMap[uint( vertices[i - 1].second )];
            hasDuplicates = true;
        } else
        { duplicatesMap[uint( vertices[i].second )] = vertices[i].second; }
    }

    return hasDuplicates;
}

void removeDuplicates( Ra::Core::Geometry::TriangleMesh& mesh, std::vector<Index>& vertexMap ) {
    std::vector<Index> duplicatesMap;
    findDuplicates( mesh, duplicatesMap );

    const size_t numVerts = mesh.vertices().size();
    std::vector<Index> newIndices( numVerts, Index::Invalid() );
    Ra::Core::Vector3Array uniqueVertices;
    Ra::Core::Vector3Array uniqueNormals;
    for ( uint i = 0; i < numVerts; ++i )
    {
        if ( duplicatesMap[i] == i )
        {
            newIndices[i] = int( uniqueVertices.size() );
            uniqueVertices.push_back( mesh.vertices()[i] );
            uniqueNormals.push_back( mesh.normals()[i] );
        }
    }

    const size_t numTri = mesh.m_indices.size();
#pragma omp parallel for schedule( static )
    for ( int i = 0; i < int( numTri ); ++i )
    {
        for ( uint j = 0; j < 3; ++j )
        {
            uint oldIdx = mesh.m_indices[uint( i )]( j );
            int newIdx = newIndices[uint( duplicatesMap[oldIdx] )];
            mesh.m_indices[uint( i )]( j ) = uint( newIdx );
        }
    }

    vertexMap.resize( numVerts );
#pragma omp parallel for schedule( static )
    for ( int i = 0; i < int( numVerts ); ++i )
        vertexMap[uint( i )] = newIndices[uint( duplicatesMap[uint( i )] )];

    mesh.setVertices( uniqueVertices );
    mesh.setNormals( uniqueNormals );
}

void MainWindow::onFrameComplete() {
    tab_edition->updateValues();
    // update timeline only if time changed, to allow manipulation of keyframed objects
    auto engine = Ra::Engine::RadiumEngine::getInstance();
    if ( !Ra::Core::Math::areApproxEqual( m_timeline->getTime(), engine->getTime() ) )
    {
        m_lockTimeSystem = true;
        m_timeline->onChangeCursor( engine->getTime() );
        m_lockTimeSystem = false;
    }
    if ( m_exportMeshes )
    {
        Ra::IO::OBJFileManager obj;

        static std::map<std::string, asset::loader::Point_cache_file> mdd_files;

        auto roMngr = Engine::RadiumEngine::getInstance()->getRenderObjectManager();
        for ( auto ro : roMngr->getRenderObjects() )
        {
            const std::shared_ptr<Engine::Displayable>& displ = ro->getMesh();
            const Engine::Mesh* mesh = dynamic_cast<Engine::Mesh*>( displ.get() );

            // Here we can filter which mesh we want:
            //   - Skeleton (why not)
            //   - IS result (whatever the version)
            //   - HRBF (marching cubes of it)
            //   - SDF
            if ( ro->getComponent()->getName().find("AC_") == std::string::npos &&
                 ro->getName().find("ImplicitSkinning") == std::string::npos &&
                 ro->getName().find("MarchingCubes") == std::string::npos &&
                 ro->getName().find("SDF_") == std::string::npos )
                continue;

            std::stringstream filenameStream;
            filenameStream << mainApp->getExportFolderName() << "/radiummesh_"
                           << ro->getName() << "_" << std::setw( 6 )
                           << std::setfill( '0' ) << mainApp->getFrameCount();
            std::string filename = filenameStream.str();

            if ( mesh == nullptr )
            {
                LOG( logERROR ) << "Render Object " << ro->getName() << " has no mesh!";
            }
            // remove duplicates before export! (not enough, vertex index changed!)
            auto triMesh = mesh->getCoreGeometry();
            std::vector<Index> dupliMap;
            removeDuplicates( triMesh, dupliMap );
            if ( obj.save( filename, triMesh ) )
            {
                LOG( logINFO ) << "Mesh from " << ro->getName() << " successfully exported to "
                               << filename;
            }
            else
            { LOG( logERROR ) << "Mesh from " << ro->getName() << "failed to export"; }

            // add a frame to the corresponding mdd file and export it
            // warning: the file will be overriden at each frame!
            auto it = mdd_files.find( ro->getName() );
            if ( it == mdd_files.end() )
            {
                // first frame: init file with number of vertices
                mdd_files.emplace( std::make_pair( ro->getName(), asset::loader::Point_cache_file( triMesh.vertices().size(), 100 ) ) );
            }
            // fill the file frame
            const auto& V = triMesh.vertices();
            std::vector<float> vertices( V.size() * 3 );
#pragma omp parallel for
            for ( int i = 0; i < V.size(); ++i )
            {
                vertices[3*i] = V[i].x();
                vertices[3*i+1] = V[i].y();
                vertices[3*i+2] = V[i].z();
            }
            // add and export the whole
            mdd_files[ro->getName()].add_frame( vertices.data() );
            mdd_files[ro->getName()].export_mdd( mainApp->getExportFolderName() + "/" + ro->getName() + ".mdd" );
        }
    }
}

void MainWindow::addRenderer( const std::string& name, std::shared_ptr<Engine::Renderer> e ) {
    int id = m_viewer->addRenderer( e );
    CORE_UNUSED( id );
    CORE_ASSERT( id == m_currentRendererCombo->count(), "Inconsistent renderer state" );
    m_currentRendererCombo->addItem( QString::fromStdString( name ) );
}

// macros to call TimeSystem's (if it exists) method X and potentially ask for
// Viewer's update or continuous update.

void MainWindow::on_actionPlay_triggered( bool checked ) {
    Ra::Engine::RadiumEngine::getInstance()->play( checked );
    mainApp->setContinuousUpdate( checked );
}

void MainWindow::on_actionStop_triggered() {
    Ra::Engine::RadiumEngine::getInstance()->resetTime();
    mainApp->askForUpdate();
    actionPlay->setChecked( false );
}

void MainWindow::on_actionStep_triggered() {
    Ra::Engine::RadiumEngine::getInstance()->step();
    mainApp->askForUpdate();
}

void MainWindow::timelinePlay( bool play ) {
    actionPlay->setChecked( play );
    if ( !m_lockTimeSystem ) { 
        Ra::Engine::RadiumEngine::getInstance()->play( play );
        mainApp->setContinuousUpdate( play );
    }
}

void MainWindow::timelineGoTo( double t ) {
    if ( !m_lockTimeSystem ) {
        Ra::Engine::RadiumEngine::getInstance()->setTime( Scalar( t ) );
        mainApp->askForUpdate();
    }
}

void MainWindow::timelineStartChanged( double t ) {
    if ( !m_lockTimeSystem ) { 
        Ra::Engine::RadiumEngine::getInstance()->setStartTime( Scalar( t ) ); 
    	mainApp->askForUpdate();
    }
}

void MainWindow::timelineEndChanged( double t ) {
    if ( !m_lockTimeSystem ) { 
        Ra::Engine::RadiumEngine::getInstance()->setEndTime( Scalar( t ) ); 
        mainApp->askForUpdate();
    }
}

void MainWindow::timelineSetPingPong( bool status ) {
    if ( !m_lockTimeSystem ) { 
        Ra::Engine::RadiumEngine::getInstance()->setForwardBackward( status );
        mainApp->askForUpdate();
    }
}

void MainWindow::onItemAdded( const Engine::ItemEntry& ent ) {
    m_itemModel->addItem( ent );
}

void MainWindow::onItemRemoved( const Engine::ItemEntry& ent ) {
    m_itemModel->removeItem( ent );
}

void MainWindow::exportCurrentMesh() {
    std::stringstream filenameStream;
    filenameStream << mainApp->getExportFolderName() << "/radiummesh_" << std::setw( 6 )
                   << std::setfill( '0' ) << mainApp->getFrameCount();
    std::string filename = filenameStream.str();

    ItemEntry e = m_selectionManager->currentItem();

    // For now we only export a mesh if the selected entry is a render object.
    // There could be a virtual method to get a mesh representation for any object.
    if ( e.isRoNode() )
    {
        Ra::IO::OBJFileManager obj;
        auto ro = Engine::RadiumEngine::getInstance()->getRenderObjectManager()->getRenderObject(
            e.m_roIndex );
        const std::shared_ptr<Engine::Displayable>& displ = ro->getMesh();
        const Engine::Mesh* mesh = dynamic_cast<Engine::Mesh*>( displ.get() );

        if ( mesh != nullptr && obj.save( filename, mesh->getCoreGeometry() ) )
        {
            LOG( logINFO ) << "Mesh from " << ro->getName() << " successfully exported to "
                           << filename;
        }
        else
        { LOG( logERROR ) << "Mesh from " << ro->getName() << "failed to export"; }
    }
    else
    { LOG( logWARNING ) << "Current entry was not a render object. No mesh was exported."; }
}

void MainWindow::exportMeshEveryFrame( bool on ) {
    m_exportMeshes = on;
}

void MainWindow::deleteCurrentItem() {
    ItemEntry e = m_selectionManager->currentItem();

    // This call is very important to avoid a potential race condition
    // which happens if an object is selected while a gizmo is present.
    // If we do not do this, the removal of the object will call ItemModel::removeItem() which
    // will cause it to be unselected by the selection model. This in turn will cause
    // the gizmos ROs to disappear, but the RO mutex is already acquired by the call for
    // the object we want to delete, which causes a deadlock.
    // Clearing the selection before deleting the object will avoid this problem.
    m_selectionManager->clear();
    if ( e.isRoNode() ) { e.m_component->removeRenderObject( e.m_roIndex ); }
    else if ( e.isComponentNode() )
    { e.m_entity->removeComponent( e.m_component->getName() ); }
    else if ( e.isEntityNode() )
    {
        Engine::RadiumEngine::getInstance()->getEntityManager()->removeEntity(
            e.m_entity->getIndex() );
    }
    mainApp->askForUpdate();
}

void MainWindow::resetScene() {
    // Fix issue #378 : ask the viewer to switch back to the default camera
    m_viewer->getCameraManipulator()->resetToDefaultCamera();
    // To see why this call is important, please see deleteCurrentItem().
    m_selectionManager->clear();
    Engine::RadiumEngine::getInstance()->getEntityManager()->deleteEntities();
    fitCamera();
}

void MainWindow::fitCamera() {
    auto aabb = Engine::RadiumEngine::getInstance()->computeSceneAabb();
    if ( aabb.isEmpty() )
    {
        m_viewer->getCameraManipulator()->resetCamera();
        mainApp->askForUpdate();
    }
    else
        m_viewer->fitCameraToScene( aabb );
}

void MainWindow::postLoadFile( const std::string& filename ) {
    m_viewer->getRenderer()->buildAllRenderTechniques();
    m_selectionManager->clear();
    m_currentShaderBox->clear();
    m_currentShaderBox->setEnabled( false );
    m_currentShaderBox->addItem( "" ); // add empty item
    for ( const auto& ro :
          Engine::RadiumEngine::getInstance()->getRenderObjectManager()->getRenderObjects() )
    {
        if ( ro->getType() == Engine::RenderObjectType::Geometry )
        {
            auto material                 = ro->getMaterial();
            const std::string& shaderName = material->getMaterialName();
            m_currentShaderBox->addItem( QString( shaderName.c_str() ) );
        }
    }

    fitCamera();

    // TODO : find a better way to activate loaded camera
    // If a camera is in the loaded scene, use it, else, use default
    std::string loadedEntityName = Core::Utils::getBaseName( filename, false );
    auto rootEntity =
        Engine::RadiumEngine::getInstance()->getEntityManager()->getEntity( loadedEntityName );
    if ( rootEntity != nullptr )
    {
        auto fc = std::find_if(
            rootEntity->getComponents().begin(),
            rootEntity->getComponents().end(),
            []( const auto& c ) { return ( c->getName().compare( 0, 7, "CAMERA_" ) == 0 ); } );
        if ( fc != rootEntity->getComponents().end() )
        {
            LOG( logINFO ) << "Activating camera " << ( *fc )->getName();

            const auto systemEntity = Ra::Engine::SystemEntity::getInstance();
            systemEntity->removeComponent( "CAMERA_DEFAULT" );

            auto camera = static_cast<Ra::Engine::Camera*>( ( *fc ).get() );
            m_viewer->getCameraManipulator()->setCamera(
                camera->duplicate( systemEntity, "CAMERA_DEFAULT" ) );
        }
    }
}

void MainWindow::onGLInitialized() {
    // Connection to gizmos after their creation
    connect( actionToggle_Local_Global,
             &QAction::toggled,
             m_viewer->getGizmoManager(),
             &GizmoManager::setLocal );
    connect(
        this, &MainWindow::selectedItem, m_viewer->getGizmoManager(), &GizmoManager::setEditable );

    // set default renderer once OpenGL is configured
    std::shared_ptr<Engine::Renderer> e( new Engine::ForwardRenderer() );
    addRenderer( "Forward Renderer", e );
}

void MainWindow::addPluginPath() {
    QString dir = QFileDialog::getExistingDirectory( this,
                                                     tr( "Open Directory" ),
                                                     "",
                                                     QFileDialog::ShowDirsOnly |
                                                         QFileDialog::DontResolveSymlinks );
    LOG( logINFO ) << "Adding the directory " << dir.toStdString() << " to the plugin directories.";
    mainApp->addPluginDirectory( dir.toStdString() );
}

void MainWindow::clearPluginPaths() {
    mainApp->clearPluginDirectories();
}

} // namespace Gui
} // namespace Ra

void Ra::Gui::MainWindow::on_m_currentColorButton_clicked() {
    // get the default color or an already existing one
    auto defColor     = Core::Utils::Color::linearRGBTosRGB( m_viewer->getBackgroundColor() );
    auto currentColor = QColor::fromRgb(
        defColor.rgb()[0] * 255, defColor.rgb()[1] * 255, defColor.rgb()[2] * 255 );
    QColor c = QColorDialog::getColor( currentColor, this, "Renderer background color" );
    if ( c.isValid() ) { updateBackgroundColor( c ); }
}
