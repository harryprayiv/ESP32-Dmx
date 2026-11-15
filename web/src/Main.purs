-- | The console page: grand master, a bank of eight channel faders and the
-- | keypad command line. It talks to the firmware over the /ws WebSocket
-- | with the binary protocol described in firmware/main/Console.h.
module Main (main) where

import Prelude

import Data.Array as Array
import Data.ArrayBuffer.Typed as Typed
import Data.ArrayBuffer.Types (ArrayBuffer, Uint8Array)
import Data.Either (Either(..))
import Data.Foldable (for_, traverse_)
import Data.Int as Int
import Data.Maybe (Maybe(..))
import Data.Set as Set
import Data.Time.Duration (Milliseconds(..))
import Data.UInt as UInt
import Effect (Effect)
import Effect.Aff (delay)
import Effect.Aff.Class (class MonadAff)
import Effect.Class (liftEffect)
import Foreign (tagOf, unsafeFromForeign)
import Halogen as H
import Halogen.Aff as HA
import Halogen.HTML as HH
import Halogen.HTML.Events as HE
import Halogen.HTML.Properties as HP
import Halogen.Query.Event (eventListener)
import Halogen.VDom.Driver (runUI)
import Keypad (Digit(..), Key(..), keyLabel, parseCommand, showKeys, slotCount)
import Web.HTML (window)
import Web.HTML.Location as Location
import Web.HTML.Window as Window
import Web.Socket.BinaryType as BinaryType
import Web.Socket.Event.EventTypes as SocketEvent
import Web.Socket.Event.MessageEvent (MessageEvent)
import Web.Socket.Event.MessageEvent as MessageEvent
import Web.Socket.WebSocket (WebSocket)
import Web.Socket.WebSocket as WebSocket

main :: Effect Unit
main = HA.runHalogenAff do
  body <- HA.awaitBody
  void (runUI component unit body)

-- Wire protocol ---------------------------------------------------------

encodeSetSlots :: Array Int -> Int -> Array Int
encodeSetSlots slots level =
  [ 0x01 ] <> Array.concatMap (\slot -> [ slot `div` 256, slot `mod` 256, level ]) slots

encodeSetMaster :: Int -> Array Int
encodeSetMaster level = [ 0x02, level ]

encodeRequestState :: Array Int
encodeRequestState = [ 0x03 ]

decodeState :: Array Int -> Maybe { master :: Int, levels :: Array Int }
decodeState message = do
  { head: opcode, tail: payload } <- Array.uncons message
  { head: master, tail: levels } <- Array.uncons payload
  if opcode == 0x81 && Array.length levels == slotCount then Just { master, levels }
  else Nothing

sendBytes :: WebSocket -> Array Int -> Effect Unit
sendBytes socket bytes = do
  view :: Uint8Array <- Typed.fromArray (map UInt.fromInt bytes)
  WebSocket.sendArrayBufferView socket view

-- | The bytes of a binary message. Nothing for a text message.
readBytes :: MessageEvent -> Effect (Maybe (Array Int))
readBytes event =
  let
    payload = MessageEvent.data_ event
  in
    if tagOf payload == "ArrayBuffer" then do
      view :: Uint8Array <- Typed.whole (unsafeFromForeign payload :: ArrayBuffer)
      bytes <- Typed.toArray view
      pure (Just (map UInt.toInt bytes))
    else pure Nothing

-- State -----------------------------------------------------------------

-- | Only an open socket can be sent on, and it only exists inside Online.
data Link
  = Connecting
  | Online WebSocket
  | Offline

type State =
  { link :: Link
  , listeners :: Array H.SubscriptionId
  , levels :: Array Int
  , master :: Int
  , bank :: Int
  , entry :: Array Key
  , notice :: String
  }

data Action
  = Connect
  | SocketOpened WebSocket
  | SocketClosed
  | SocketMessage MessageEvent
  | MasterMoved String
  | FaderMoved Int String
  | BankMoved Int
  | KeyPressed Key
  | DeletePressed
  | EnterPressed
  | BlackoutPressed

fadersPerBank :: Int
fadersPerBank = 8

bankCount :: Int
bankCount = slotCount / fadersPerBank

initialState :: State
initialState =
  { link: Connecting
  , listeners: []
  , levels: Array.replicate slotCount 0
  , master: 255
  , bank: 0
  , entry: []
  , notice: ""
  }

parseLevel :: String -> Maybe Int
parseLevel text = do
  level <- Int.fromString text
  if level >= 0 && level <= 255 then Just level else Nothing

setLevels :: Array Int -> Int -> Array Int -> Array Int
setLevels slots level levels =
  let
    chosen = Set.fromFoldable slots
  in
    Array.mapWithIndex (\slot old -> if Set.member slot chosen then level else old) levels

levelToPercent :: Int -> Int
levelToPercent level = Int.round (Int.toNumber level * 100.0 / 255.0)

-- Component -------------------------------------------------------------

component :: forall query input output m. MonadAff m => H.Component query input output m
component =
  H.mkComponent
    { initialState: const initialState
    , render
    , eval: H.mkEval H.defaultEval { handleAction = handleAction, initialize = Just Connect }
    }

-- | Runs the action only while the socket is open. The page changes its own
-- | levels in the same step that sends them, so what it shows cannot drift
-- | from what the firmware was told.
whenOnline
  :: forall output m
   . (WebSocket -> H.HalogenM State Action () output m Unit)
  -> H.HalogenM State Action () output m Unit
whenOnline act = H.gets _.link >>= case _ of
  Online socket -> act socket
  _ -> pure unit

handleAction :: forall output m. MonadAff m => Action -> H.HalogenM State Action () output m Unit
handleAction = case _ of
  Connect -> do
    socket <- liftEffect do
      host <- window >>= Window.location >>= Location.host
      socket <- WebSocket.create ("ws://" <> host <> "/ws") []
      WebSocket.setBinaryType socket BinaryType.ArrayBuffer
      pure socket
    let target = WebSocket.toEventTarget socket
    opened <- H.subscribe (eventListener SocketEvent.onOpen target \_ -> Just (SocketOpened socket))
    message <- H.subscribe (eventListener SocketEvent.onMessage target (map SocketMessage <<< MessageEvent.fromEvent))
    closed <- H.subscribe (eventListener SocketEvent.onClose target \_ -> Just SocketClosed)
    H.modify_ _ { link = Connecting, listeners = [ opened, message, closed ] }

  SocketOpened socket -> do
    H.modify_ _ { link = Online socket }
    liftEffect (sendBytes socket encodeRequestState)

  SocketClosed -> do
    listeners <- H.gets _.listeners
    traverse_ H.unsubscribe listeners
    H.modify_ _ { link = Offline, listeners = [] }
    H.liftAff (delay (Milliseconds 1000.0))
    handleAction Connect

  SocketMessage event -> do
    bytes <- liftEffect (readBytes event)
    for_ (bytes >>= decodeState) \state ->
      H.modify_ _ { master = state.master, levels = state.levels }

  MasterMoved text -> whenOnline \socket ->
    for_ (parseLevel text) \level -> do
      H.modify_ _ { master = level }
      liftEffect (sendBytes socket (encodeSetMaster level))

  FaderMoved slot text -> whenOnline \socket ->
    for_ (parseLevel text) \level -> do
      H.modify_ \state -> state { levels = setLevels [ slot ] level state.levels }
      liftEffect (sendBytes socket (encodeSetSlots [ slot ] level))

  BankMoved delta ->
    H.modify_ \state -> state { bank = clamp 0 (bankCount - 1) (state.bank + delta) }

  KeyPressed key ->
    H.modify_ \state -> state { entry = Array.snoc state.entry key, notice = "" }

  DeletePressed ->
    H.modify_ \state -> state { entry = Array.dropEnd 1 state.entry, notice = "" }

  EnterPressed -> whenOnline \socket -> do
    entry <- H.gets _.entry
    case parseCommand entry of
      Left problem -> H.modify_ _ { notice = problem }
      Right command -> do
        H.modify_ \state -> state
          { levels = setLevels command.slots command.level state.levels
          , entry = []
          , notice = showKeys entry
          }
        liftEffect (sendBytes socket (encodeSetSlots command.slots command.level))

  BlackoutPressed -> whenOnline \socket -> do
    H.modify_ \state -> state { levels = map (const 0) state.levels, notice = "Blackout" }
    liftEffect (sendBytes socket (encodeSetSlots (Array.range 0 (slotCount - 1)) 0))

-- View ------------------------------------------------------------------

type View m = H.ComponentHTML Action () m

css :: forall r i. String -> HH.IProp (class :: String | r) i
css = HP.class_ <<< HH.ClassName

render :: forall m. State -> View m
render state =
  HH.div [ css "console" ]
    [ HH.header_
        [ HH.h1_ [ HH.text "ESP32 DMX" ]
        , HH.span [ css ("link " <> link.style) ] [ HH.text link.text ]
        , HH.button
            [ css "blackout", HP.disabled offline, HE.onClick \_ -> BlackoutPressed ]
            [ HH.text "Blackout" ]
        ]
    , HH.section [ css "faders" ]
        ( [ fader offline "master" "GM" state.master MasterMoved
          , HH.div [ css "bank" ]
              [ HH.button [ HP.disabled (state.bank == 0), HE.onClick \_ -> BankMoved (-1) ] [ HH.text "Prev" ]
              , HH.span_ [ HH.text (show (firstSlot + 1) <> " to " <> show (firstSlot + fadersPerBank)) ]
              , HH.button [ HP.disabled (state.bank == bankCount - 1), HE.onClick \_ -> BankMoved 1 ] [ HH.text "Next" ]
              ]
          ]
            <> Array.mapWithIndex channelFader (Array.slice firstSlot (firstSlot + fadersPerBank) state.levels)
        )
    , HH.section [ css "command" ]
        [ if Array.null state.entry then HH.div [ css "entry hint" ] [ HH.text "Chan [Thru Chan] @ Level" ]
          else HH.div [ css "entry" ] [ HH.text (showKeys state.entry) ]
        , HH.div [ css "notice" ] [ HH.text state.notice ]
        ]
    , HH.section [ css "keypad" ] (map (keyButton offline) keypad)
    ]
  where
  firstSlot = state.bank * fadersPerBank

  offline = case state.link of
    Online _ -> false
    _ -> true

  link = case state.link of
    Connecting -> { style: "connecting", text: "connecting" }
    Online _ -> { style: "online", text: "online" }
    Offline -> { style: "offline", text: "offline" }

  channelFader index level =
    fader offline "channel" (show (firstSlot + index + 1)) level (FaderMoved (firstSlot + index))

-- | min and max are set before value on purpose. A range input clamps its
-- | value to the limits it has at that moment, and the default maximum is 100.
fader :: forall m. Boolean -> String -> String -> Int -> (String -> Action) -> View m
fader disabled style name level moved =
  HH.label [ css ("fader " <> style) ]
    [ HH.span [ css "name" ] [ HH.text name ]
    , HH.input
        [ HP.type_ HP.InputRange
        , HP.min 0.0
        , HP.max 255.0
        , HP.value (show level)
        , HP.disabled disabled
        , HE.onValueInput moved
        ]
    , HH.span [ css "value" ] [ HH.text (show level) ]
    , HH.span [ css "percent" ] [ HH.text (show (levelToPercent level) <> "%") ]
    ]

type KeyCap =
  { label :: String
  , style :: String
  , action :: Action
  }

-- | The Pockonsole layout. Its S key had no function in the sketch, so that
-- | position holds DEL, which removes the last key from the command line.
keypad :: Array KeyCap
keypad =
  [ typed (Digit D1), typed (Digit D2), typed (Digit D3), typed And
  , typed (Digit D4), typed (Digit D5), typed (Digit D6), typed Minus
  , typed (Digit D7), typed (Digit D8), typed (Digit D9), { label: "DEL", style: "edit", action: DeletePressed }
  , typed At, typed (Digit D0), typed Thru, { label: "E", style: "enter", action: EnterPressed }
  ]
  where
  typed key =
    { label: keyLabel key
    , style: case key of
        Digit _ -> "digit"
        _ -> "operator"
    , action: KeyPressed key
    }

keyButton :: forall m. Boolean -> KeyCap -> View m
keyButton disabled cap =
  HH.button
    [ css ("key " <> cap.style), HP.disabled disabled, HE.onClick \_ -> cap.action ]
    [ HH.text cap.label ]
